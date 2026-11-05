#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "sdkconfig.h"
#include "alog.h"
#include "hci_log.h"

#define ALOG_TEXT        0
#define ALOG_RING_BYTES  (24 * 1024)
#define ALOG_LINE_MAX    256
#ifdef CONFIG_DONGLE_LOG_HEX_MAX_BYTES
#define ALOG_CAPTURE_MAX (CONFIG_DONGLE_LOG_HEX_MAX_BYTES > 8 ? CONFIG_DONGLE_LOG_HEX_MAX_BYTES : 8)
#else
#define ALOG_CAPTURE_MAX 8
#endif

typedef struct {
    uint8_t kind;
    uint32_t seq;
    uint16_t len;      /* original packet length */
    const char *dir;
    uint8_t data[];    /* min(len, ALOG_CAPTURE_MAX) bytes */
} hci_rec_t;

static RingbufHandle_t s_ring;
static TaskHandle_t s_task;
static vprintf_like_t s_orig_vprintf;
static volatile uint32_t s_dropped;
static volatile uint32_t s_seq;
static uint32_t s_cur_seq;

uint32_t alog_current_seq(void)
{
    return s_cur_seq;
}

static int alog_vprintf(const char *fmt, va_list ap)
{
    if (s_ring == NULL || xPortInIsrContext() || xTaskGetCurrentTaskHandle() == s_task ||
        xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) {
        return s_orig_vprintf(fmt, ap);
    }
    char buf[1 + ALOG_LINE_MAX];
    buf[0] = ALOG_TEXT;
    int n = vsnprintf(buf + 1, ALOG_LINE_MAX, fmt, ap);
    if (n < 0) {
        return n;
    }
    size_t out = n < ALOG_LINE_MAX ? (size_t)n : ALOG_LINE_MAX - 1;
    if (xRingbufferSend(s_ring, buf, 1 + out, 0) != pdTRUE) {
        s_dropped++;
    }
    return n;
}

void alog_submit_hci(alog_kind_t kind, const char *dir, const uint8_t *data, size_t len)
{
    if (s_ring == NULL) {
        return;
    }
    size_t cap = len < ALOG_CAPTURE_MAX ? len : ALOG_CAPTURE_MAX;
    hci_rec_t *rec;
    if (xRingbufferSendAcquire(s_ring, (void **)&rec, sizeof(*rec) + cap, 0) != pdTRUE) {
        s_dropped++;
        return;
    }
    rec->kind = kind;
    rec->seq = __atomic_add_fetch(&s_seq, 1, __ATOMIC_RELAXED);
    rec->len = len > UINT16_MAX ? UINT16_MAX : (uint16_t)len;
    rec->dir = dir;
    memcpy(rec->data, data, cap);
    xRingbufferSendComplete(s_ring, rec);
}

static void emit_text(const char *s, size_t n)
{
    char line[ALOG_LINE_MAX + 1];
    memcpy(line, s, n);
    line[n] = '\0';
    fputs(line, stdout);
}

static void alog_task(void *arg)
{
    uint32_t reported = 0;
    for (;;) {
        size_t size;
        uint8_t *item = xRingbufferReceive(s_ring, &size, pdMS_TO_TICKS(200));
        if (item != NULL) {
            if (item[0] == ALOG_TEXT) {
                emit_text((const char *)item + 1, size - 1);
            } else {
                const hci_rec_t *r = (const hci_rec_t *)item;
                s_cur_seq = r->seq;
                switch (r->kind) {
                case ALOG_HCI_CMD: hci_log_cmd_now(r->dir, r->data, r->len); break;
                case ALOG_HCI_EVT: hci_log_evt_now(r->dir, r->data, r->len); break;
                case ALOG_HCI_ACL: hci_log_acl_now(r->dir, r->data, r->len); break;
                case ALOG_HCI_HEX: hci_log_hex_now(r->dir, r->data, r->len); break;
                default: break;
                }
            }
            vRingbufferReturnItem(s_ring, item);
        }
        uint32_t d = s_dropped;
        if (d != reported) {
            ESP_LOGW("ALOG", "%lu log record(s) dropped (ring full)", (unsigned long)(d - reported));
            reported = d;
        }
    }
}

void alog_start(void)
{
    s_ring = xRingbufferCreateWithCaps(ALOG_RING_BYTES, RINGBUF_TYPE_NOSPLIT, MALLOC_CAP_INTERNAL);
    if (s_ring == NULL) {
        ESP_LOGE("ALOG", "ring buffer allocation failed, logging stays synchronous");
        return;
    }
    s_orig_vprintf = esp_log_set_vprintf(alog_vprintf);
    xTaskCreatePinnedToCore(alog_task, "alog", 6144, NULL, 1, &s_task, 1);
    ESP_LOGI("ALOG", "async logging on core 1 (ring %d bytes)", ALOG_RING_BYTES);
}
