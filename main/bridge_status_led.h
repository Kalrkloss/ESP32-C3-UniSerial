#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t bridge_status_led_start(bool usb_mode);
void bridge_status_led_set_telnet_connected(bool connected);
void bridge_status_led_activity(void);
