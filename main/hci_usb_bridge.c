#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_bt.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "tusb.h"
#include "device/usbd_pvt.h"
#include "hci_log.h"
#include "status_led.h"

#define CONN_MAX 16
#include "hci_usb_bridge.h"

static const char *TAG = "BRIDGE";

#define H4_CMD   0x01
#define H4_ACL   0x02
#define H4_SCO   0x03
#define H4_EVT   0x04
#define H4_ISO   0x05

#define ACL_MAX_LEN        1024
#define H2C_QUEUE_DEPTH    32
#define EVT_QUEUE_DEPTH    64
#define ACL_QUEUE_DEPTH    32
#define VHCI_STALL_WARN_MS 1000

typedef struct {
    uint16_t len;
    uint16_t pad;
    uint8_t data[];
} pkt_t;

typedef struct {
    uint32_t cmd_rx, acl_rx, h2c_sent, h2c_dropped;
    uint32_t evt_rx, acl_tx_rx, evt_sent, acl_sent, c2h_dropped, c2h_unsupported;
    uint32_t vhci_waits, acl_reasm_errors, usb_tx_errors;
} stats_t;

static stats_t s_stats;
static QueueHandle_t s_h2c_q;
static QueueHandle_t s_evt_q;
static QueueHandle_t s_acl_q;
static SemaphoreHandle_t s_vhci_ready;

/* USB IN endpoint state, only touched from the TinyUSB task */
static pkt_t *s_evt_inflight;
static pkt_t *s_acl_inflight;
static volatile bool s_pump_pending;

/* Host -> controller ACL reassembly across 64-byte USB packets */
static pkt_t *s_acl_asm;
static uint16_t s_acl_asm_got;

#define STAT_INC(field) __atomic_fetch_add(&s_stats.field, 1, __ATOMIC_RELAXED)

static pkt_t *pkt_alloc(uint16_t len)
{
    pkt_t *p = heap_caps_malloc(sizeof(pkt_t) + len, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (p) {
        p->len = len;
    }
    return p;
}

static void pkt_free(pkt_t *p)
{
    heap_caps_free(p);
}

static void queue_flush(QueueHandle_t q)
{
    pkt_t *p;
    while (xQueueReceive(q, &p, 0) == pdTRUE) {
        pkt_free(p);
    }
}

/* ---------------- controller -> host ---------------- */

static void usb_tx_pump(void *arg);

static void usb_tx_kick(void)
{
    if (!__atomic_exchange_n(&s_pump_pending, true, __ATOMIC_ACQ_REL)) {
        usbd_defer_func(usb_tx_pump, NULL, false);
    }
}

static uint16_t s_conn_handles[CONN_MAX];
static unsigned s_conn_count;

static void conn_publish(void)
{
    status_led_set_connections(s_conn_count);
}

static void conn_track_event(const uint8_t *e, uint16_t len)
{
    if (e[0] == 0x3E && len >= 8 && (e[2] == 0x01 || e[2] == 0x0A) && e[3] == 0x00) {
        uint16_t h = (e[4] | (e[5] << 8)) & 0x0FFF;
        if (s_conn_count < CONN_MAX) {
            s_conn_handles[s_conn_count++] = h;
        }
    } else if (e[0] == 0x05 && len >= 5 && e[2] == 0x00) {
        uint16_t h = (e[3] | (e[4] << 8)) & 0x0FFF;
        for (unsigned i = 0; i < s_conn_count; i++) {
            if (s_conn_handles[i] == h) {
                s_conn_handles[i] = s_conn_handles[--s_conn_count];
                break;
            }
        }
    } else if (e[0] == 0x0E && len >= 6 && e[3] == 0x03 && e[4] == 0x0C && e[5] == 0x00) {
        s_conn_count = 0; /* HCI Reset drops every connection */
    } else {
        return;
    }
    conn_publish();
}

static int vhci_host_recv(uint8_t *data, uint16_t len)
{
    if (len < 1) {
        return 0;
    }
    uint8_t type = data[0];
    const uint8_t *body = data + 1;
    uint16_t blen = len - 1;

    if (type == H4_EVT) {
        STAT_INC(evt_rx);
        conn_track_event(body, blen);
        hci_log_evt("C->H", body, blen);
    } else if (type == H4_ACL) {
        STAT_INC(acl_tx_rx);
        status_led_activity();
        hci_log_acl("C->H", body, blen);
    } else {
        STAT_INC(c2h_unsupported);
        ESP_LOGW(TAG, "C->H unsupported H4 type 0x%02X (%u bytes), dropped", type, len);
        return 0;
    }

    pkt_t *p = pkt_alloc(blen);
    if (!p) {
        STAT_INC(c2h_dropped);
        ESP_LOGE(TAG, "C->H out of memory (%u bytes), packet dropped", blen);
        return 0;
    }
    memcpy(p->data, body, blen);

    /* Classic (OGF 1-3) write-type commands the LE-only controller rejects with "Unknown HCI Command":
     * report success so Windows' BR/EDR bring-up sequence completes */
    if (type == H4_EVT && blen == 6 && body[0] == 0x0E && body[1] == 0x04 && body[5] == 0x01) {
        uint16_t op = body[3] | (body[4] << 8);
        uint8_t ogf = op >> 10;
        if (ogf >= 1 && ogf <= 3) {
            ESP_LOGI(TAG, "faking success for unsupported classic opcode 0x%04X", op);
            p->data[5] = 0x00;
        }
    }

    /* Advertise BR/EDR support (bit 5 of byte 4 cleared) so Windows accepts the radio */
    if (type == H4_EVT && blen >= 14 && body[0] == 0x0E && body[3] == 0x03 && body[4] == 0x10 && body[5] == 0x00) {
        static const uint8_t features[8] = {0xBF, 0xFE, 0x8F, 0xFE, 0xD8, 0x3F, 0x5B, 0x07};
        ESP_LOGI(TAG, "patching Read Local Supported Features to advertise BR/EDR + LE");
        hci_log_hex("features(orig)", body + 6, 8);
        memcpy(p->data + 6, features, sizeof(features));
    }

    QueueHandle_t q = (type == H4_EVT) ? s_evt_q : s_acl_q;
    if (xQueueSend(q, &p, 0) != pdTRUE) {
        STAT_INC(c2h_dropped);
        ESP_LOGW(TAG, "C->H %s queue full, packet dropped", type == H4_EVT ? "event" : "ACL");
        pkt_free(p);
        return 0;
    }
    usb_tx_kick();
    return 0;
}

static void vhci_send_available(void)
{
    ESP_LOGV(TAG, "VHCI send available");
    xSemaphoreGive(s_vhci_ready);
}

static const esp_vhci_host_callback_t s_vhci_cb = {
    .notify_host_send_available = vhci_send_available,
    .notify_host_recv = vhci_host_recv,
};

static void usb_tx_pump(void *arg)
{
    s_pump_pending = false;

    if (!tud_ready()) {
        pkt_t *p;
        while (xQueueReceive(s_evt_q, &p, 0) == pdTRUE || xQueueReceive(s_acl_q, &p, 0) == pdTRUE) {
            STAT_INC(c2h_dropped);
            ESP_LOGW(TAG, "USB not ready, dropping %u byte packet", p->len);
            pkt_free(p);
        }
        return;
    }

    if (!s_evt_inflight && xQueueReceive(s_evt_q, &s_evt_inflight, 0) == pdTRUE) {
        if (tud_bt_event_send(s_evt_inflight->data, s_evt_inflight->len)) {
            ESP_LOGV(TAG, "USB EVT IN submitted (%u bytes)", s_evt_inflight->len);
        } else {
            STAT_INC(usb_tx_errors);
            ESP_LOGE(TAG, "tud_bt_event_send failed (%u bytes)", s_evt_inflight->len);
            pkt_free(s_evt_inflight);
            s_evt_inflight = NULL;
        }
    }

    if (!s_acl_inflight && xQueueReceive(s_acl_q, &s_acl_inflight, 0) == pdTRUE) {
        if (tud_bt_acl_data_send(s_acl_inflight->data, s_acl_inflight->len)) {
            ESP_LOGV(TAG, "USB ACL IN submitted (%u bytes)", s_acl_inflight->len);
        } else {
            STAT_INC(usb_tx_errors);
            ESP_LOGE(TAG, "tud_bt_acl_data_send failed (%u bytes)", s_acl_inflight->len);
            pkt_free(s_acl_inflight);
            s_acl_inflight = NULL;
        }
    }
}

void tud_bt_event_sent_cb(uint16_t sent_bytes)
{
    STAT_INC(evt_sent);
    ESP_LOGV(TAG, "USB EVT IN done (%u bytes)", sent_bytes);
    if (s_evt_inflight) {
        pkt_free(s_evt_inflight);
        s_evt_inflight = NULL;
    }
    usb_tx_pump(NULL);
}

void tud_bt_acl_data_sent_cb(uint16_t sent_bytes)
{
    STAT_INC(acl_sent);
    ESP_LOGV(TAG, "USB ACL IN done (%u bytes)", sent_bytes);
    if (s_acl_inflight) {
        pkt_free(s_acl_inflight);
        s_acl_inflight = NULL;
    }
    usb_tx_pump(NULL);
}

/* ---------------- host -> controller ---------------- */

/* Windows insists on a BR/EDR capable controller. The ESP32-S3 controller is LE only and rejects classic
 * commands, so the bridge answers the ones Windows needs during bring-up with plausible static values. */
static void evt_inject(const uint8_t *evt, uint16_t len)
{
    pkt_t *p = pkt_alloc(len);
    if (!p) {
        STAT_INC(c2h_dropped);
        ESP_LOGE(TAG, "SHIM out of memory");
        return;
    }
    memcpy(p->data, evt, len);
    hci_log_evt("SHIM", p->data, len);
    if (xQueueSend(s_evt_q, &p, 0) != pdTRUE) {
        STAT_INC(c2h_dropped);
        ESP_LOGW(TAG, "SHIM event queue full");
        pkt_free(p);
        return;
    }
    usb_tx_kick();
}

static bool shim_handle_cmd(uint16_t opcode)
{
    /* Classic "Read_*" commands the LE-only controller lacks: success plus zeroed return parameters */
    static const struct {
        uint16_t opcode;
        uint8_t rp_len;
    } read_cmds[] = {
        {0x0C14, 248}, /* Read Local Name */
        {0x0C17, 2},   /* Read Page Timeout */
        {0x0C19, 1},   /* Read Scan Enable */
        {0x0C1B, 4},   /* Read Page Scan Activity */
        {0x0C1D, 4},   /* Read Inquiry Scan Activity */
        {0x0C1F, 1},   /* Read Authentication Enable */
        {0x0C23, 3},   /* Read Class of Device */
        {0x0C25, 2},   /* Read Voice Setting */
        {0x0C38, 1},   /* Read Number Of Supported IAC */
        {0x0C44, 1},   /* Read Inquiry Mode */
        {0x0C46, 1},   /* Read Page Scan Type */
        {0x0C42, 1},   /* Read Inquiry Scan Type */
        {0x0C51, 241}, /* Read Extended Inquiry Response */
        {0x0C55, 1},   /* Read Simple Pairing Mode */
        {0x0C58, 1},   /* Read Inquiry Response Transmit Power Level */
        {0x080E, 2},   /* Read Default Link Policy Settings */
    };

    for (size_t i = 0; i < sizeof(read_cmds) / sizeof(read_cmds[0]); i++) {
        if (read_cmds[i].opcode == opcode) {
            uint8_t evt[7 + 255] = {0x0E, 4 + read_cmds[i].rp_len, 0x05, opcode & 0xFF, opcode >> 8, 0x00};
            evt_inject(evt, 6 + read_cmds[i].rp_len);
            return true;
        }
    }

    switch (opcode) {
    case 0x1005: { /* Read Buffer Size (BR/EDR) */
        const uint8_t evt[] = {
            0x0E, 0x0B, 0x05, opcode & 0xFF, opcode >> 8, 0x00,
            0xFD, 0x03,       /* ACL data packet length 1021 */
            0x3C,             /* SCO data packet length 60 */
            0x04, 0x00,       /* total ACL packets */
            0x01, 0x00,       /* total SCO packets */
        };
        evt_inject(evt, sizeof(evt));
        return true;
    }
    default:
        return false;
    }
}

static void h2c_enqueue(uint8_t type, const uint8_t *body, uint16_t blen)
{
    pkt_t *p = pkt_alloc(blen + 1);
    if (!p) {
        STAT_INC(h2c_dropped);
        ESP_LOGE(TAG, "H->C out of memory (%u bytes), packet dropped", blen);
        return;
    }
    p->data[0] = type;
    memcpy(p->data + 1, body, blen);
    if (xQueueSend(s_h2c_q, &p, 0) != pdTRUE) {
        STAT_INC(h2c_dropped);
        ESP_LOGW(TAG, "H->C queue full, packet dropped");
        pkt_free(p);
    }
}

void tud_bt_hci_cmd_cb(void *hci_cmd, size_t cmd_len)
{
    STAT_INC(cmd_rx);
    const uint8_t *b = hci_cmd;
    hci_log_cmd("H->C", b, cmd_len);
    if (cmd_len < 3 || (size_t)b[2] + 3 > cmd_len) {
        ESP_LOGW(TAG, "H->C malformed command (%u bytes), dropped", (unsigned)cmd_len);
        return;
    }
    uint16_t opcode = b[0] | (b[1] << 8);
    if (shim_handle_cmd(opcode)) {
        ESP_LOGI(TAG, "H->C opcode 0x%04X answered by shim (not forwarded)", opcode);
        return;
    }
    h2c_enqueue(H4_CMD, b, b[2] + 3);
}

static void acl_asm_reset(void)
{
    s_acl_asm_got = 0;
}

void tud_bt_acl_data_received_cb(void *acl_data, uint16_t data_len)
{
    const uint8_t *b = acl_data;
    ESP_LOGV(TAG, "USB ACL OUT chunk (%u bytes)", data_len);
    status_led_activity();

    if (data_len == 0) {
        return; /* ZLP terminating a transfer whose size was a multiple of the packet size */
    }
    if (s_acl_asm_got == 0 && data_len < 4) {
        STAT_INC(acl_reasm_errors);
        ESP_LOGW(TAG, "H->C ACL chunk too short for header (%u bytes), dropped", data_len);
        return;
    }
    if (s_acl_asm_got == 0) {
        uint16_t total = 4 + (b[2] | (b[3] << 8));
        if (total > ACL_MAX_LEN) {
            STAT_INC(acl_reasm_errors);
            ESP_LOGE(TAG, "H->C ACL length %u exceeds %u, dropped", total, ACL_MAX_LEN);
            return;
        }
        s_acl_asm->len = total;
    }
    if (s_acl_asm_got + data_len > ACL_MAX_LEN) {
        STAT_INC(acl_reasm_errors);
        ESP_LOGE(TAG, "H->C ACL reassembly overflow, resetting");
        acl_asm_reset();
        return;
    }
    memcpy(s_acl_asm->data + s_acl_asm_got, b, data_len);
    s_acl_asm_got += data_len;

    if (s_acl_asm_got < s_acl_asm->len) {
        ESP_LOGV(TAG, "H->C ACL reassembling %u/%u", s_acl_asm_got, s_acl_asm->len);
        return;
    }
    if (s_acl_asm_got > s_acl_asm->len) {
        STAT_INC(acl_reasm_errors);
        ESP_LOGW(TAG, "H->C ACL got %u bytes, expected %u; trailing bytes ignored", s_acl_asm_got, s_acl_asm->len);
    }
    STAT_INC(acl_rx);
    hci_log_acl("H->C", s_acl_asm->data, s_acl_asm->len);
    h2c_enqueue(H4_ACL, s_acl_asm->data, s_acl_asm->len);
    acl_asm_reset();
}

static void vhci_tx_task(void *arg)
{
    for (;;) {
        pkt_t *p;
        if (xQueueReceive(s_h2c_q, &p, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        int64_t t0 = esp_timer_get_time();
        bool warned = false;
        while (!esp_vhci_host_check_send_available()) {
            STAT_INC(vhci_waits);
            xSemaphoreTake(s_vhci_ready, pdMS_TO_TICKS(10));
            if (!warned && (esp_timer_get_time() - t0) / 1000 > VHCI_STALL_WARN_MS) {
                ESP_LOGW(TAG, "controller not ready for >%d ms (type 0x%02X)", VHCI_STALL_WARN_MS, p->data[0]);
                warned = true;
            }
        }
        esp_vhci_host_send_packet(p->data, p->len);
        STAT_INC(h2c_sent);
        ESP_LOGV(TAG, "VHCI send type 0x%02X, %u bytes, waited %lld us", p->data[0], p->len,
                 (long long)(esp_timer_get_time() - t0));
        pkt_free(p);
    }
}

/* ---------------- housekeeping ---------------- */

static void stats_task(void *arg)
{
    uint32_t report = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(CONFIG_DONGLE_STATS_PERIOD_MS));
        ESP_LOGI(TAG, "stats #%lu H->C: cmd=%lu acl=%lu sent=%lu drop=%lu reasm_err=%lu | C->H: evt=%lu acl=%lu "
                 "usb_evt=%lu usb_acl=%lu drop=%lu unsup=%lu usb_err=%lu | vhci_waits=%lu",
                 (unsigned long)++report, (unsigned long)s_stats.cmd_rx, (unsigned long)s_stats.acl_rx, (unsigned long)s_stats.h2c_sent,
                 (unsigned long)s_stats.h2c_dropped, (unsigned long)s_stats.acl_reasm_errors,
                 (unsigned long)s_stats.evt_rx, (unsigned long)s_stats.acl_tx_rx, (unsigned long)s_stats.evt_sent,
                 (unsigned long)s_stats.acl_sent, (unsigned long)s_stats.c2h_dropped,
                 (unsigned long)s_stats.c2h_unsupported, (unsigned long)s_stats.usb_tx_errors,
                 (unsigned long)s_stats.vhci_waits);
        ESP_LOGI(TAG, "queues #%lu h2c=%u evt=%u acl=%u | usb_ready=%d | heap free=%lu min=%lu dma=%lu",
                 (unsigned long)report, (unsigned)uxQueueMessagesWaiting(s_h2c_q), (unsigned)uxQueueMessagesWaiting(s_evt_q),
                 (unsigned)uxQueueMessagesWaiting(s_acl_q), tud_ready(),
                 (unsigned long)esp_get_free_heap_size(), (unsigned long)esp_get_minimum_free_heap_size(),
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_DMA));
    }
}

void hci_usb_bridge_usb_event(tinyusb_event_t *event, void *arg)
{
    switch (event->id) {
    case TINYUSB_EVENT_ATTACHED:
        ESP_LOGI(TAG, "USB: configured by host (mounted)");
        break;
    case TINYUSB_EVENT_DETACHED:
        ESP_LOGW(TAG, "USB: unmounted/detached, flushing packet queues");
        if (s_evt_inflight) { pkt_free(s_evt_inflight); s_evt_inflight = NULL; }
        if (s_acl_inflight) { pkt_free(s_acl_inflight); s_acl_inflight = NULL; }
        queue_flush(s_evt_q);
        queue_flush(s_acl_q);
        acl_asm_reset();
        break;
    case TINYUSB_EVENT_SUSPENDED:
        ESP_LOGW(TAG, "USB: bus suspended (remote wakeup %s)",
                 event->suspended.remote_wakeup ? "enabled" : "disabled");
        break;
    case TINYUSB_EVENT_RESUMED:
        ESP_LOGI(TAG, "USB: bus resumed");
        break;
    default:
        ESP_LOGW(TAG, "USB: unknown event %d", event->id);
        break;
    }
}

esp_err_t hci_usb_bridge_start(void)
{
    s_h2c_q = xQueueCreate(H2C_QUEUE_DEPTH, sizeof(pkt_t *));
    s_evt_q = xQueueCreate(EVT_QUEUE_DEPTH, sizeof(pkt_t *));
    s_acl_q = xQueueCreate(ACL_QUEUE_DEPTH, sizeof(pkt_t *));
    s_vhci_ready = xSemaphoreCreateBinary();
    s_acl_asm = pkt_alloc(ACL_MAX_LEN);
    if (!s_h2c_q || !s_evt_q || !s_acl_q || !s_vhci_ready || !s_acl_asm) {
        ESP_LOGE(TAG, "allocation failure during bridge init");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_vhci_host_register_callback(&s_vhci_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_vhci_host_register_callback failed: %s", esp_err_to_name(err));
        return err;
    }
    xTaskCreate(vhci_tx_task, "vhci_tx", 4096, NULL, 10, NULL);
    if (CONFIG_DONGLE_STATS_PERIOD_MS > 0) {
        xTaskCreate(stats_task, "stats", 3072, NULL, 3, NULL);
    }
    ESP_LOGI(TAG, "bridge ready (queues h2c=%d evt=%d acl=%d)", H2C_QUEUE_DEPTH, EVT_QUEUE_DEPTH, ACL_QUEUE_DEPTH);
    return ESP_OK;
}
