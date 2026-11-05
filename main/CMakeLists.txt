idf_component_register(SRCS "main.c" "alog.c" "hci_usb_bridge.c" "hci_log.c" "usb_bt_desc.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES bt nvs_flash esp_timer esp_system esp_hw_support esp_rom esp_ringbuf)
