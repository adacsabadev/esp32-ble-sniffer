#pragma once

#include "tinyusb.h"

esp_err_t hci_usb_bridge_start(void);
void hci_usb_bridge_usb_event(tinyusb_event_t *event, void *arg);
