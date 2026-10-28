#pragma once

#include "app_config.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "freertos/queue.h"

esp_err_t telnet_bridge_start(const app_config_t *config, uart_port_t uart_port,
                              QueueHandle_t uart_queue);
