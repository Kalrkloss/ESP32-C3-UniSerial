#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define APP_WIFI_SSID_MAX 32
#define APP_WIFI_PASSWORD_MAX 64
#define APP_UART_BUFFER_MIN 64
#define APP_UART_BUFFER_MAX 2048
#define APP_UART_BAUD_MIN 1200
#define APP_UART_BAUD_MAX 3000000

typedef enum {
  APP_UART_PARITY_NONE,
  APP_UART_PARITY_EVEN,
  APP_UART_PARITY_ODD,
} app_uart_parity_t;

typedef struct {
  char wifi_ssid[APP_WIFI_SSID_MAX + 1];
  char wifi_password[APP_WIFI_PASSWORD_MAX + 1];
  uint32_t uart_baud_rate;
  uint16_t uart_buffer_size;
  uint8_t uart_data_bits;
  uint8_t uart_stop_bits;
  app_uart_parity_t uart_parity;
  bool uart_rx_invert;
  bool uart_tx_invert;
  uint8_t wifi_ap_channel;
} app_config_t;

void app_config_set_defaults(app_config_t *config);
bool app_config_is_valid(const app_config_t *config);
esp_err_t app_config_load(app_config_t *config);
esp_err_t app_config_save(const app_config_t *config);
