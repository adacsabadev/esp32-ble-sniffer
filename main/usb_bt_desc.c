#include <stdio.h>
#include "esp_log.h"
#include "esp_mac.h"
#include "sdkconfig.h"
#include "tusb.h"
#include "tinyusb_default_config.h"
#include "usb_bt_desc.h"

static const char *TAG = "USB_DESC";

#define EP_EVT_IN     0x81
#define EP_ACL_IN     0x82
#define EP_ACL_OUT    0x02
#define EP_EVT_SIZE   64
#define EP_ACL_SIZE   64
#define EP_EVT_POLL   1

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_BTH_DESC_LEN)

static const tusb_desc_device_t s_device_desc = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    /* Class/subclass/protocol E0/01/01 makes Windows bind its inbox Bluetooth radio driver */
    .bDeviceClass = TUSB_CLASS_WIRELESS_CONTROLLER,
    .bDeviceSubClass = 0x01,
    .bDeviceProtocol = 0x01,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = CONFIG_DONGLE_USB_VID,
    .idProduct = CONFIG_DONGLE_USB_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

static const uint8_t s_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, CONFIG_TOTAL_LEN, 0, 250),
    /* Interface 1 carries the standard SCO/ISO alternate settings (sizes per alt 0..5) that Windows' BTHUSB expects.
     * They are only advertised: this BLE-only dongle never services the isochronous endpoints. */
    TUD_BTH_DESCRIPTOR(0, 0, EP_EVT_IN, EP_EVT_SIZE, EP_EVT_POLL, EP_ACL_IN, EP_ACL_OUT, EP_ACL_SIZE,
                       0, 9, 17, 25, 33, 49),
};

static char s_serial[13];

static const char *s_strings[] = {
    (const char[]){0x09, 0x04},
    "Espressif",
    "ESP32-S3 BLE HCI Dongle",
    s_serial,
};

esp_err_t usb_bt_start(tinyusb_event_cb_t event_cb)
{
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_BT));
    snprintf(s_serial, sizeof(s_serial), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "VID:PID %04X:%04X serial %s", CONFIG_DONGLE_USB_VID, CONFIG_DONGLE_USB_PID, s_serial);
    ESP_LOGI(TAG, "config descriptor %u bytes: EVT IN 0x%02X (%u B, %u ms) ACL IN 0x%02X ACL OUT 0x%02X (%u B)",
             (unsigned)sizeof(s_config_desc), EP_EVT_IN, EP_EVT_SIZE, EP_EVT_POLL, EP_ACL_IN, EP_ACL_OUT, EP_ACL_SIZE);

    tinyusb_config_t cfg = TINYUSB_DEFAULT_CONFIG(event_cb, NULL);
    cfg.descriptor.device = &s_device_desc;
    cfg.descriptor.string = s_strings;
    cfg.descriptor.string_count = sizeof(s_strings) / sizeof(s_strings[0]);
    cfg.descriptor.full_speed_config = s_config_desc;
    return tinyusb_driver_install(&cfg);
}
