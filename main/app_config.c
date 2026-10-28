#include "app_config.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "sdkconfig.h"

#define CONFIG_NAMESPACE "unis"

static const char *TAG = "app_config";

void app_config_set_defaults(app_config_t *config) {
  memset(config, 0, sizeof(*config));
  config->uart_baud_rate = CONFIG_UBRIDGE_UART_BAUD_RATE;
  config->uart_buffer_size = 512;
  config->uart_data_bits = 8;
  config->uart_stop_bits = 1;
  config->wifi_ap_channel = 1;
  config->telnet_port = APP_TELNET_PORT_DEFAULT;
#if CONFIG_UBRIDGE_UART_PARITY == 2
  config->uart_parity = APP_UART_PARITY_EVEN;
#elif CONFIG_UBRIDGE_UART_PARITY == 3
  config->uart_parity = APP_UART_PARITY_ODD;
#else
  config->uart_parity = APP_UART_PARITY_NONE;
#endif
}

bool app_config_is_valid(const app_config_t *config) {
  if (config == NULL ||
      strnlen(config->wifi_ssid, sizeof(config->wifi_ssid)) >
          APP_WIFI_SSID_MAX ||
      strnlen(config->wifi_password, sizeof(config->wifi_password)) >
          APP_WIFI_PASSWORD_MAX ||
      strnlen(config->telnet_username, sizeof(config->telnet_username)) >
          APP_TELNET_USERNAME_MAX ||
      strnlen(config->telnet_password, sizeof(config->telnet_password)) >
          APP_TELNET_PASSWORD_MAX ||
      config->uart_baud_rate < APP_UART_BAUD_MIN ||
      config->uart_baud_rate > APP_UART_BAUD_MAX ||
      config->uart_buffer_size < APP_UART_BUFFER_MIN ||
      config->uart_buffer_size > APP_UART_BUFFER_MAX ||
      (config->uart_buffer_size & (config->uart_buffer_size - 1)) != 0 ||
      config->uart_data_bits < 5 || config->uart_data_bits > 8 ||
      (config->uart_stop_bits != 1 && config->uart_stop_bits != 15 &&
       config->uart_stop_bits != 2) ||
      config->uart_parity > APP_UART_PARITY_ODD ||
      config->wifi_ap_channel < 1 || config->wifi_ap_channel > 13 ||
      config->telnet_port == 0 || config->telnet_port == 80) {
    return false;
  }

  size_t password_len = strnlen(config->wifi_password,
                                sizeof(config->wifi_password));
  size_t telnet_password_len =
      strnlen(config->telnet_password, sizeof(config->telnet_password));
  return (password_len == 0 || password_len >= 8) &&
         (telnet_password_len == 0 || config->telnet_username[0] != '\0');
}

static esp_err_t read_string(nvs_handle_t handle, const char *key,
                             char *value, size_t capacity) {
  size_t length = capacity;
  esp_err_t err = nvs_get_str(handle, key, value, &length);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    value[0] = '\0';
    return ESP_OK;
  }
  return err;
}

static esp_err_t read_u32(nvs_handle_t handle, const char *key,
                          uint32_t *value) {
  esp_err_t err = nvs_get_u32(handle, key, value);
  return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
}

static esp_err_t read_u8(nvs_handle_t handle, const char *key,
                         uint8_t *value) {
  esp_err_t err = nvs_get_u8(handle, key, value);
  return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
}

esp_err_t app_config_load(app_config_t *config) {
  if (config == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  app_config_set_defaults(config);
  nvs_handle_t handle;
  esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READONLY, &handle);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    return ESP_OK;
  }
  if (err != ESP_OK) {
    return err;
  }

  uint8_t parity = config->uart_parity;
  uint8_t rx_invert = 0;
  uint8_t tx_invert = 0;
  uint32_t buffer_size = config->uart_buffer_size;
  uint8_t telnet_enabled = 0;
  uint32_t telnet_port = config->telnet_port;
  err = read_string(handle, "ssid", config->wifi_ssid,
                    sizeof(config->wifi_ssid));
  if (err == ESP_OK) {
    err = read_string(handle, "wifi_pass", config->wifi_password,
                      sizeof(config->wifi_password));
  }
  if (err == ESP_OK) {
    err = read_string(handle, "telnet_user", config->telnet_username,
                      sizeof(config->telnet_username));
  }
  if (err == ESP_OK) {
    err = read_string(handle, "telnet_pass", config->telnet_password,
                      sizeof(config->telnet_password));
  }
  if (err == ESP_OK) {
    err = read_u32(handle, "baud", &config->uart_baud_rate);
  }
  if (err == ESP_OK) {
    err = read_u32(handle, "buf_size", &buffer_size);
  }
  if (err == ESP_OK) {
    err = read_u8(handle, "data_bits", &config->uart_data_bits);
  }
  if (err == ESP_OK) {
    err = read_u8(handle, "stop_bits", &config->uart_stop_bits);
  }
  if (err == ESP_OK) {
    err = read_u8(handle, "parity", &parity);
  }
  if (err == ESP_OK) {
    err = read_u8(handle, "rx_invert", &rx_invert);
  }
  if (err == ESP_OK) {
    err = read_u8(handle, "tx_invert", &tx_invert);
  }
  if (err == ESP_OK) {
    err = read_u8(handle, "ap_channel", &config->wifi_ap_channel);
  }
  if (err == ESP_OK) {
    err = read_u8(handle, "telnet_enabled", &telnet_enabled);
  }
  if (err == ESP_OK) {
    err = read_u32(handle, "telnet_port", &telnet_port);
  }
  nvs_close(handle);
  if (err != ESP_OK) {
    return err;
  }

  if (buffer_size > UINT16_MAX || telnet_port > UINT16_MAX) {
    ESP_LOGW(TAG, "Stored configuration value is invalid; using defaults");
    app_config_set_defaults(config);
    return ESP_OK;
  }
  config->uart_parity = (app_uart_parity_t)parity;
  config->uart_rx_invert = rx_invert != 0;
  config->uart_tx_invert = tx_invert != 0;
  config->uart_buffer_size = (uint16_t)buffer_size;
  config->telnet_enabled = telnet_enabled != 0;
  config->telnet_port = (uint16_t)telnet_port;
  if (!app_config_is_valid(config)) {
    ESP_LOGW(TAG, "Stored settings are invalid; using defaults");
    app_config_set_defaults(config);
  }
  return ESP_OK;
}

esp_err_t app_config_save(const app_config_t *config) {
  if (!app_config_is_valid(config)) {
    return ESP_ERR_INVALID_ARG;
  }

  nvs_handle_t handle;
  esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle);
  if (err != ESP_OK) {
    return err;
  }

  err = nvs_set_str(handle, "ssid", config->wifi_ssid);
  if (err == ESP_OK) {
    err = nvs_set_str(handle, "wifi_pass", config->wifi_password);
  }
  if (err == ESP_OK) {
    err = nvs_set_u32(handle, "baud", config->uart_baud_rate);
  }
  if (err == ESP_OK) {
    err = nvs_set_u32(handle, "buf_size", config->uart_buffer_size);
  }
  if (err == ESP_OK) {
    err = nvs_set_u8(handle, "data_bits", config->uart_data_bits);
  }
  if (err == ESP_OK) {
    err = nvs_set_u8(handle, "stop_bits", config->uart_stop_bits);
  }
  if (err == ESP_OK) {
    err = nvs_set_u8(handle, "parity", config->uart_parity);
  }
  if (err == ESP_OK) {
    err = nvs_set_u8(handle, "rx_invert", config->uart_rx_invert);
  }
  if (err == ESP_OK) {
    err = nvs_set_u8(handle, "tx_invert", config->uart_tx_invert);
  }
  if (err == ESP_OK) {
    err = nvs_set_u8(handle, "ap_channel", config->wifi_ap_channel);
  }
  if (err == ESP_OK) {
    err = nvs_set_u8(handle, "telnet_enabled", config->telnet_enabled);
  }
  if (err == ESP_OK) {
    err = nvs_set_u32(handle, "telnet_port", config->telnet_port);
  }
  if (err == ESP_OK) {
    err = nvs_set_str(handle, "telnet_user", config->telnet_username);
  }
  if (err == ESP_OK) {
    err = nvs_set_str(handle, "telnet_pass", config->telnet_password);
  }
  if (err == ESP_OK) {
    err = nvs_commit(handle);
  }

  nvs_close(handle);
  return err;
}
