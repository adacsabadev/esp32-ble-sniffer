#include "esp_bt.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "alog.h"
#include "hci_usb_bridge.h"
#include "status_led.h"
#include "usb_bt_desc.h"

static const char *TAG = "MAIN";

static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "interrupt WDT";
    case ESP_RST_TASK_WDT:  return "task WDT";
    case ESP_RST_WDT:       return "other WDT";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_USB:       return "USB";
    default:                return "other";
    }
}

static void log_banner(void)
{
    esp_chip_info_t info;
    esp_chip_info(&info);
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);

    ESP_LOGI(TAG, "=== ESP32-S3 BLE USB HCI dongle ===");
    ESP_LOGI(TAG, "IDF %s, chip rev %u.%u, %u core(s)", esp_get_idf_version(), info.revision / 100,
             info.revision % 100, info.cores);
    ESP_LOGI(TAG, "reset reason: %s", reset_reason_str(esp_reset_reason()));
    ESP_LOGI(TAG, "BT MAC %02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "heap free=%lu dma=%lu", (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_DMA));
}

#define CHECK(step, expr)                                                          \
    do {                                                                           \
        ESP_LOGI(TAG, "%s ...", step);                                             \
        esp_err_t _e = (expr);                                                     \
        if (_e != ESP_OK) {                                                        \
            ESP_LOGE(TAG, "%s failed: %s (0x%x)", step, esp_err_to_name(_e), _e);  \
            return;                                                                \
        }                                                                          \
    } while (0)

void app_main(void)
{
    alog_start();
    status_led_start();
    esp_log_level_set("HCI", ESP_LOG_VERBOSE);
    esp_log_level_set("BRIDGE", ESP_LOG_VERBOSE);
    log_banner();

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase (%s)", esp_err_to_name(ret));
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    CHECK("BT controller init", esp_bt_controller_init(&bt_cfg));
    CHECK("BT controller enable (BLE)", esp_bt_controller_enable(ESP_BT_MODE_BLE));
    CHECK("HCI/USB bridge start", hci_usb_bridge_start());
    CHECK("USB device start", usb_bt_start(hci_usb_bridge_usb_event));

    ESP_LOGI(TAG, "dongle running; plug the native USB port into the Windows host");
}
