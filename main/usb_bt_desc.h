#pragma once

#include "tinyusb.h"

/* Fills in USB descriptors (serial number derived from the base MAC) and installs TinyUSB. */
esp_err_t usb_bt_start(tinyusb_event_cb_t event_cb);
