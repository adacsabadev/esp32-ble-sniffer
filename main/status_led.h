#pragma once

#include "esp_err.h"

esp_err_t status_led_start(void);

/* Non-blocking, callable from any task: LED is blue while count > 0, off otherwise. */
void status_led_set_connections(unsigned count);

/* Non-blocking: call on every BLE data packet; the LED blinks briefly while a device is connected. */
void status_led_activity(void);
