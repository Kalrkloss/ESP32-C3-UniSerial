#include "wifi_portal.h"

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#define WIFI_GOT_IP_BIT BIT0
#define WIFI_DISCONNECTED_BIT BIT1
#define WIFI_CONNECT_TIMEOUT_MS 15000
#define WIFI_RETRY_LIMIT 3
#define WIFI_AP_MAX_CONNECTIONS 4
#define DNS_PORT 53
#define DNS_PACKET_MAX 512
#define MAX_SCAN_RESULTS 20
#define MAX_FORM_SIZE 1024

static const char *TAG = "wifi_portal";
static app_config_t s_config;
static EventGroupHandle_t s_wifi_events;
static esp_netif_t *s_ap_netif;
static esp_netif_t *s_sta_netif;
static httpd_handle_t s_http_server;
static wifi_ap_record_t s_scan_records[MAX_SCAN_RESULTS];
static wifi_ap_record_t s_scan_batch[MAX_SCAN_RESULTS];
static bool s_wifi_started;
static bool s_dns_started;

static uint16_t read_be16(const uint8_t *data) {
  return ((uint16_t)data[0] << 8) | data[1];
}

static void write_be16(uint8_t *data, uint16_t value) {
  data[0] = (uint8_t)(value >> 8);
  data[1] = (uint8_t)value;
}

static void write_be32(uint8_t *data, uint32_t value) {
  data[0] = (uint8_t)(value >> 24);
  data[1] = (uint8_t)(value >> 16);
  data[2] = (uint8_t)(value >> 8);
  data[3] = (uint8_t)value;
}

static int build_dns_reply(const uint8_t *request, size_t request_len,
                           uint8_t *reply, size_t reply_capacity,
                           uint32_t ap_ip) {
  if (request_len < 12 || request_len > DNS_PACKET_MAX ||
      reply_capacity < request_len || (read_be16(request + 2) & 0x8000) != 0 ||
      (read_be16(request + 2) & 0x7800) != 0 ||
      read_be16(request + 4) != 1) {
    return -1;
  }

  size_t offset = 12;
  size_t name_length = 0;
  while (offset < request_len) {
    uint8_t label_length = request[offset++];
    if (label_length == 0) {
      break;
    }
    if ((label_length & 0xc0) != 0 || label_length > 63 ||
        offset + label_length > request_len ||
        name_length + label_length + 1 > 255) {
      return -1;
    }
    offset += label_length;
    name_length += label_length + 1;
  }
  if (offset + 4 > request_len) {
    return -1;
  }

  uint16_t query_type = read_be16(request + offset);
  uint16_t query_class = read_be16(request + offset + 2);
  size_t question_end = offset + 4;
  bool answer_a_record = query_type == 1 && query_class == 1;
  size_t answer_length = answer_a_record ? 16 : 0;
  if (question_end + answer_length > reply_capacity) {
    return -1;
  }

  memcpy(reply, request, question_end);
  uint16_t query_flags = read_be16(request + 2);
  uint16_t response_flags =
      0x8000 | 0x0400 | (query_flags & 0x0100); // response, authoritative, RD
  write_be16(reply + 2, response_flags);
  write_be16(reply + 4, 1);
  write_be16(reply + 6, answer_a_record ? 1 : 0);
  write_be16(reply + 8, 0);
  write_be16(reply + 10, 0);

  if (!answer_a_record) {
    return (int)question_end;
  }

  uint8_t *answer = reply + question_end;
  answer[0] = 0xc0;
  answer[1] = 0x0c;
  write_be16(answer + 2, 1);
  write_be16(answer + 4, 1);
  write_be32(answer + 6, 30);
  write_be16(answer + 10, 4);
  memcpy(answer + 12, &ap_ip, sizeof(ap_ip));
  return (int)(question_end + answer_length);
}

static void dns_server_task(void *arg) {
  (void)arg;
  uint8_t request[DNS_PACKET_MAX];
  uint8_t reply[DNS_PACKET_MAX];

  for (;;) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
      ESP_LOGE(TAG, "DNS socket creation failed: errno=%d", errno);
      vTaskDelay(pdMS_TO_TICKS(2000));
      continue;
    }

    struct sockaddr_in local_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0) {
      ESP_LOGE(TAG, "DNS bind failed: errno=%d", errno);
      close(sock);
      vTaskDelay(pdMS_TO_TICKS(2000));
      continue;
    }
    ESP_LOGI(TAG, "Captive DNS listening on UDP port %d", DNS_PORT);

    for (;;) {
      struct sockaddr_in peer_addr;
      socklen_t peer_len = sizeof(peer_addr);
      int received = recvfrom(sock, request, sizeof(request), 0,
                              (struct sockaddr *)&peer_addr, &peer_len);
      if (received < 0) {
        ESP_LOGE(TAG, "DNS receive failed: errno=%d", errno);
        break;
      }

      esp_netif_ip_info_t ip_info;
      esp_err_t err = esp_netif_get_ip_info(s_ap_netif, &ip_info);
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not read setup AP address: %s",
                 esp_err_to_name(err));
        continue;
      }

      int reply_len = build_dns_reply(request, received, reply, sizeof(reply),
                                      ip_info.ip.addr);
      if (reply_len <= 0) {
        continue;
      }

      int sent = sendto(sock, reply, reply_len, 0,
                        (struct sockaddr *)&peer_addr, peer_len);
      if (sent != reply_len) {
        ESP_LOGW(TAG, "DNS response send failed: sent=%d expected=%d", sent,
                 reply_len);
      }
    }

    shutdown(sock, 0);
    close(sock);
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

static esp_err_t start_dns_server(void) {
  if (s_dns_started) {
    return ESP_OK;
  }
  BaseType_t created =
      xTaskCreate(dns_server_task, "captive_dns", 4096, NULL, 5, NULL);
  if (created != pdPASS) {
    return ESP_ERR_NO_MEM;
  }
  s_dns_started = true;
  return ESP_OK;
}

static esp_err_t send_format(httpd_req_t *req, const char *format, ...) {
  char buffer[256];
  va_list args;
  va_start(args, format);
  int length = vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  if (length < 0 || (size_t)length >= sizeof(buffer)) {
    return ESP_ERR_INVALID_SIZE;
  }
  return httpd_resp_sendstr_chunk(req, buffer);
}

static esp_err_t send_html_escaped(httpd_req_t *req, const char *text) {
  char output[256];
  size_t output_length = 0;
  for (const unsigned char *cursor = (const unsigned char *)text; *cursor;
       cursor++) {
    const char *escaped = NULL;
    switch (*cursor) {
    case '&':
      escaped = "&amp;";
      break;
    case '<':
      escaped = "&lt;";
      break;
    case '>':
      escaped = "&gt;";
      break;
    case '"':
      escaped = "&quot;";
      break;
    case '\'':
      escaped = "&#39;";
      break;
    default:
      break;
    }

    const char *source = escaped != NULL ? escaped : (const char *)cursor;
    size_t source_length = escaped != NULL ? strlen(escaped) : 1;
    if (output_length + source_length >= sizeof(output)) {
      output[output_length] = '\0';
      esp_err_t err = httpd_resp_sendstr_chunk(req, output);
      if (err != ESP_OK) {
        return err;
      }
      output_length = 0;
    }
    memcpy(output + output_length, source, source_length);
    output_length += source_length;
  }
  if (output_length > 0) {
    output[output_length] = '\0';
    return httpd_resp_sendstr_chunk(req, output);
  }
  return ESP_OK;
}

static bool scan_access_points(uint16_t *record_count) {
  uint16_t count = 0;
  bool scan_ok = true;
  for (uint8_t channel = 1; channel <= 13; channel++) {
    wifi_scan_config_t scan_config = {
        .channel = channel,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 0,
        .scan_time.active.max = 120,
        .show_hidden = true,
    };
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "Wi-Fi scan failed on channel %u: %s", channel,
               esp_err_to_name(err));
      scan_ok = false;
      break;
    }

    uint16_t found = MAX_SCAN_RESULTS;
    err = esp_wifi_scan_get_ap_records(&found, s_scan_batch);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Reading scan results failed on channel %u: %s", channel,
               esp_err_to_name(err));
      scan_ok = false;
      break;
    }
    for (uint16_t i = 0; i < found && count < MAX_SCAN_RESULTS; i++) {
      s_scan_records[count++] = s_scan_batch[i];
    }
    if (channel % 3 == 0) {
      vTaskDelay(pdMS_TO_TICKS(120));
    }
  }
  *record_count = count;
  return scan_ok;
}

static esp_err_t index_handler(httpd_req_t *req) {
  uint16_t record_count = 0;
  bool scan_ok = scan_access_points(&record_count);
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");

  esp_err_t err = httpd_resp_sendstr_chunk(
      req, "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
           "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
           "<title>ESP32-C3-UniSerial</title><style>"
           "body{font:16px system-ui;max-width:760px;margin:2rem auto;padding:0 1rem}"
           "fieldset{margin:1rem 0;padding:1rem;border:1px solid #bbb}"
           "label{display:block;margin:.7rem 0}.warning{color:#8a3400}"
           "input,select,button{font:inherit;padding:.35rem}input[type=checkbox]{width:auto}"
           "</style></head><body><h1>ESP32-C3-UniSerial</h1>"
           "<p class=\"warning\"><b>Open setup network:</b> settings and Wi-Fi "
           "credentials use unencrypted HTTP. Use only in a trusted area.</p>"
           "<form method=\"post\" action=\"/save\">"
           "<fieldset><legend>Wi-Fi</legend>"
           "<label>Scanned networks<select id=\"scan\" "
           "onchange=\"document.getElementById('ssid').value=this.value\">"
           "<option value=\"\">Select a scanned network</option>");
  if (err != ESP_OK) {
    return err;
  }

  for (uint16_t i = 0; i < record_count; i++) {
    char ssid[APP_WIFI_SSID_MAX + 1];
    memcpy(ssid, s_scan_records[i].ssid, APP_WIFI_SSID_MAX);
    ssid[APP_WIFI_SSID_MAX] = '\0';
    if (ssid[0] == '\0') {
      continue;
    }
    err = httpd_resp_sendstr_chunk(req, "<option value=\"");
    if (err == ESP_OK) {
      err = send_html_escaped(req, ssid);
    }
    if (err == ESP_OK) {
      err = httpd_resp_sendstr_chunk(req, "\">");
    }
    if (err == ESP_OK) {
      err = send_html_escaped(req, ssid);
    }
    if (err == ESP_OK) {
      err = send_format(req, " (%d dBm, ch %u)</option>",
                        s_scan_records[i].rssi, s_scan_records[i].primary);
    }
    if (err != ESP_OK) {
      return err;
    }
  }

  err = httpd_resp_sendstr_chunk(
      req, "</select></label><label>Network name (SSID)<input id=\"ssid\" "
           "name=\"ssid\" maxlength=\"32\" value=\"");
  if (err == ESP_OK) {
    err = send_html_escaped(req, s_config.wifi_ssid);
  }
  if (err == ESP_OK) {
    err = httpd_resp_sendstr_chunk(
        req, "\"></label><label>Wi-Fi password<input type=\"password\" "
             "name=\"wifi_password\" maxlength=\"64\" "
             "autocomplete=\"new-password\"></label>"
             "<label>Setup AP channel (1-13)<input type=\"number\" "
             "name=\"ap_channel\" min=\"1\" max=\"13\" value=\"");
  }
  if (err == ESP_OK) {
    err = send_format(req, "%u", s_config.wifi_ap_channel);
  }
  if (err == ESP_OK) {
    err = httpd_resp_sendstr_chunk(
        req, "\"></label><p>When connected as a station, the router selects "
             "the Wi-Fi channel.</p>");
  }
  if (err == ESP_OK && !scan_ok) {
    err = httpd_resp_sendstr_chunk(
        req, "<p>Network scan failed; enter the SSID manually.</p>");
  }
  if (err == ESP_OK) {
    err = httpd_resp_sendstr_chunk(req, "</fieldset><fieldset><legend>UART</legend>");
  }
  if (err != ESP_OK) {
    return err;
  }

  err = send_format(
      req, "<label>Baud rate<input type=\"number\" name=\"baud\" min=\"%u\" "
           "max=\"%u\" value=\"%" PRIu32 "\" required></label>"
           "<label>Buffer size<select name=\"buffer_size\">",
      APP_UART_BAUD_MIN, APP_UART_BAUD_MAX, s_config.uart_baud_rate);
  if (err != ESP_OK) {
    return err;
  }
  const uint16_t buffer_sizes[] = {64, 128, 256, 512, 1024, 2048};
  for (size_t i = 0; i < sizeof(buffer_sizes) / sizeof(buffer_sizes[0]); i++) {
    err = send_format(
        req, "<option value=\"%u\"%s>%u bytes</option>", buffer_sizes[i],
        buffer_sizes[i] == s_config.uart_buffer_size ? " selected" : "",
        buffer_sizes[i]);
    if (err != ESP_OK) {
      return err;
    }
  }
  err = httpd_resp_sendstr_chunk(
      req, "</select></label><label>Data bits<select name=\"data_bits\">");
  if (err != ESP_OK) {
    return err;
  }
  for (uint8_t bits = 5; bits <= 8; bits++) {
    err = send_format(req, "<option value=\"%u\"%s>%u</option>", bits,
                      bits == s_config.uart_data_bits ? " selected" : "",
                      bits);
    if (err != ESP_OK) {
      return err;
    }
  }

  err = httpd_resp_sendstr_chunk(
      req, "</select></label><label>Parity<select name=\"parity\">");
  if (err != ESP_OK) {
    return err;
  }
  const char *parity_names[] = {"None", "Even", "Odd"};
  for (uint8_t parity = APP_UART_PARITY_NONE; parity <= APP_UART_PARITY_ODD;
       parity++) {
    err = send_format(req, "<option value=\"%u\"%s>%s</option>", parity,
                      parity == s_config.uart_parity ? " selected" : "",
                      parity_names[parity]);
    if (err != ESP_OK) {
      return err;
    }
  }

  err = httpd_resp_sendstr_chunk(
      req, "</select></label><label>Stop bits<select name=\"stop_bits\">");
  if (err != ESP_OK) {
    return err;
  }
  const uint8_t stop_values[] = {1, 15, 2};
  const char *stop_names[] = {"1", "1.5", "2"};
  for (size_t i = 0; i < sizeof(stop_values) / sizeof(stop_values[0]); i++) {
    err = send_format(req, "<option value=\"%u\"%s>%s</option>",
                      stop_values[i],
                      stop_values[i] == s_config.uart_stop_bits ? " selected"
                                                                : "",
                      stop_names[i]);
    if (err != ESP_OK) {
      return err;
    }
  }

  err = httpd_resp_sendstr_chunk(
      req, "</select></label><label><input type=\"checkbox\" "
           "name=\"rx_invert\" value=\"1\"");
  if (err == ESP_OK && s_config.uart_rx_invert) {
    err = httpd_resp_sendstr_chunk(req, " checked");
  }
  if (err == ESP_OK) {
    err = httpd_resp_sendstr_chunk(req, "> Invert RX signal</label>"
                                       "<label><input type=\"checkbox\" "
                                       "name=\"tx_invert\" value=\"1\"");
  }
  if (err == ESP_OK && s_config.uart_tx_invert) {
    err = httpd_resp_sendstr_chunk(req, " checked");
  }
  if (err == ESP_OK) {
    err = httpd_resp_sendstr_chunk(
        req, "> Invert TX signal</label></fieldset>"
             "<button type=\"submit\">Save and restart</button></form></body></html>");
  }
  if (err != ESP_OK) {
    return err;
  }
  return httpd_resp_sendstr_chunk(req, NULL);
}

static int hex_value(char character) {
  if (character >= '0' && character <= '9') {
    return character - '0';
  }
  if (character >= 'a' && character <= 'f') {
    return character - 'a' + 10;
  }
  if (character >= 'A' && character <= 'F') {
    return character - 'A' + 10;
  }
  return -1;
}

static bool decode_component(const char *begin, size_t length, char *output,
                             size_t capacity) {
  size_t written = 0;
  for (size_t i = 0; i < length; i++) {
    unsigned char character = (unsigned char)begin[i];
    if (character == '+') {
      character = ' ';
    } else if (character == '%') {
      if (i + 2 >= length) {
        return false;
      }
      int high = hex_value(begin[i + 1]);
      int low = hex_value(begin[i + 2]);
      if (high < 0 || low < 0) {
        return false;
      }
      character = (unsigned char)((high << 4) | low);
      i += 2;
    }
    if (character == '\0' || character < 0x20 || character == 0x7f ||
        written + 1 >= capacity) {
      return false;
    }
    output[written++] = (char)character;
  }
  output[written] = '\0';
  return true;
}

static bool get_form_value(const char *body, const char *key, char *output,
                           size_t capacity) {
  size_t key_length = strlen(key);
  const char *field = body;
  while (*field != '\0') {
    const char *field_end = strchr(field, '&');
    if (field_end == NULL) {
      field_end = field + strlen(field);
    }
    const char *equals = memchr(field, '=', field_end - field);
    if (equals != NULL && (size_t)(equals - field) == key_length &&
        memcmp(field, key, key_length) == 0) {
      return decode_component(equals + 1, field_end - equals - 1, output,
                              capacity);
    }
    field = *field_end == '&' ? field_end + 1 : field_end;
  }
  return false;
}

static bool parse_uint_value(const char *value, uint32_t minimum,
                            uint32_t maximum, uint32_t *result) {
  if (value[0] == '\0') {
    return false;
  }
  errno = 0;
  char *end = NULL;
  unsigned long parsed = strtoul(value, &end, 10);
  if (errno != 0 || end == value || *end != '\0' || parsed < minimum ||
      parsed > maximum) {
    return false;
  }
  *result = (uint32_t)parsed;
  return true;
}

static esp_err_t save_handler(httpd_req_t *req) {
  if (req->content_len == 0 || req->content_len > MAX_FORM_SIZE) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                               "Invalid form size");
  }

  char body[MAX_FORM_SIZE + 1];
  size_t received = 0;
  while (received < req->content_len) {
    int length = httpd_req_recv(req, body + received,
                                req->content_len - received);
    if (length <= 0) {
      return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                 "Incomplete form data");
    }
    received += (size_t)length;
  }
  body[received] = '\0';

  app_config_t next_config = s_config;
  char value[APP_WIFI_PASSWORD_MAX + 1];
  uint32_t number;
  if (!get_form_value(body, "ssid", next_config.wifi_ssid,
                      sizeof(next_config.wifi_ssid)) ||
      !get_form_value(body, "wifi_password", next_config.wifi_password,
                      sizeof(next_config.wifi_password)) ||
      !get_form_value(body, "baud", value, sizeof(value)) ||
      !parse_uint_value(value, APP_UART_BAUD_MIN, APP_UART_BAUD_MAX, &number)) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                               "Invalid or missing Wi-Fi/baud settings");
  }
  next_config.uart_baud_rate = number;

  if (!get_form_value(body, "buffer_size", value, sizeof(value)) ||
      !parse_uint_value(value, APP_UART_BUFFER_MIN, APP_UART_BUFFER_MAX,
                        &number)) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                               "Invalid UART buffer size");
  }
  next_config.uart_buffer_size = (uint16_t)number;

  if (!get_form_value(body, "data_bits", value, sizeof(value)) ||
      !parse_uint_value(value, 5, 8, &number)) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                               "Invalid UART data bits");
  }
  next_config.uart_data_bits = (uint8_t)number;

  if (!get_form_value(body, "stop_bits", value, sizeof(value)) ||
      !parse_uint_value(value, 1, 15, &number) ||
      (number != 1 && number != 15 && number != 2)) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                               "Invalid UART stop bits");
  }
  next_config.uart_stop_bits = (uint8_t)number;

  if (!get_form_value(body, "parity", value, sizeof(value)) ||
      !parse_uint_value(value, APP_UART_PARITY_NONE, APP_UART_PARITY_ODD,
                        &number)) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                               "Invalid UART parity");
  }
  next_config.uart_parity = (app_uart_parity_t)number;

  if (!get_form_value(body, "ap_channel", value, sizeof(value)) ||
      !parse_uint_value(value, 1, 13, &number)) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                               "Invalid setup AP channel");
  }
  next_config.wifi_ap_channel = (uint8_t)number;

  next_config.uart_rx_invert =
      get_form_value(body, "rx_invert", value, sizeof(value)) &&
      strcmp(value, "1") == 0;
  next_config.uart_tx_invert =
      get_form_value(body, "tx_invert", value, sizeof(value)) &&
      strcmp(value, "1") == 0;

  if (!app_config_is_valid(&next_config)) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                               "Settings are outside supported limits");
  }

  esp_err_t err = app_config_save(&next_config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Saving configuration failed: %s", esp_err_to_name(err));
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                               "Could not save settings");
  }
  s_config = next_config;

  httpd_resp_set_type(req, "text/html; charset=utf-8");
  err = httpd_resp_sendstr(
      req, "<!doctype html><html><body><h1>Settings saved</h1>"
           "<p>The device is restarting. Reconnect in a few seconds.</p>"
           "</body></html>");
  if (err == ESP_OK) {
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
  }
  return err;
}

static esp_err_t redirect_to_portal(httpd_req_t *req,
                                    httpd_err_code_t error) {
  (void)error;
  httpd_resp_set_status(req, "302 Temporary Redirect");
  httpd_resp_set_hdr(req, "Location", "/");
  return httpd_resp_sendstr(req, "Redirecting to configuration");
}

static esp_err_t start_http_server(void) {
  if (s_http_server != NULL) {
    return ESP_OK;
  }

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.stack_size = 6144;
  config.max_open_sockets = 4;
  config.lru_purge_enable = true;
  esp_err_t err = httpd_start(&s_http_server, &config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "HTTP server start failed: %s", esp_err_to_name(err));
    return err;
  }

  const httpd_uri_t root = {
      .uri = "/",
      .method = HTTP_GET,
      .handler = index_handler,
  };
  const httpd_uri_t save = {
      .uri = "/save",
      .method = HTTP_POST,
      .handler = save_handler,
  };
  err = httpd_register_uri_handler(s_http_server, &root);
  if (err == ESP_OK) {
    err = httpd_register_uri_handler(s_http_server, &save);
  }
  if (err == ESP_OK) {
    err = httpd_register_err_handler(s_http_server, HTTPD_404_NOT_FOUND,
                                     redirect_to_portal);
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "HTTP handler registration failed: %s", esp_err_to_name(err));
    httpd_stop(s_http_server);
    s_http_server = NULL;
    return err;
  }
  ESP_LOGI(TAG, "Web configuration is available on HTTP port %d",
           config.server_port);
  return ESP_OK;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
  (void)arg;
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    const wifi_event_sta_disconnected_t *event = event_data;
    ESP_LOGW(TAG, "Station disconnected: reason=%d", event->reason);
    xEventGroupClearBits(s_wifi_events, WIFI_GOT_IP_BIT);
    xEventGroupSetBits(s_wifi_events, WIFI_DISCONNECTED_BIT);
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    const ip_event_got_ip_t *event = event_data;
    ESP_LOGI(TAG, "Connected to Wi-Fi; address=" IPSTR,
             IP2STR(&event->ip_info.ip));
    xEventGroupClearBits(s_wifi_events, WIFI_DISCONNECTED_BIT);
    xEventGroupSetBits(s_wifi_events, WIFI_GOT_IP_BIT);
  }
}

static esp_err_t configure_setup_ap(void) {
  uint8_t mac[6];
  esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
  if (err != ESP_OK) {
    return err;
  }

  wifi_config_t ap_config = {0};
  int ssid_length =
      snprintf((char *)ap_config.ap.ssid, sizeof(ap_config.ap.ssid),
               "UniSerial-%02X%02X%02X", mac[3], mac[4], mac[5]);
  if (ssid_length < 0 || ssid_length >= sizeof(ap_config.ap.ssid)) {
    return ESP_ERR_INVALID_SIZE;
  }
  ap_config.ap.ssid_len = (uint8_t)ssid_length;
  ap_config.ap.channel = s_config.wifi_ap_channel;
  ap_config.ap.max_connection = WIFI_AP_MAX_CONNECTIONS;
  ap_config.ap.authmode = WIFI_AUTH_OPEN;

  err = esp_wifi_set_mode(WIFI_MODE_APSTA);
  if (err == ESP_OK) {
    err = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
  }
  if (err == ESP_OK) {
    err = esp_wifi_start();
  }
  if (err != ESP_OK) {
    return err;
  }
  s_wifi_started = true;

  esp_netif_ip_info_t ip_info;
  err = esp_netif_get_ip_info(s_ap_netif, &ip_info);
  if (err != ESP_OK) {
    return err;
  }
  ESP_LOGW(TAG, "Open setup AP '%s' started; configure at http://" IPSTR,
           ap_config.ap.ssid, IP2STR(&ip_info.ip));
  err = start_http_server();
  if (err == ESP_OK) {
    err = start_dns_server();
  }
  return err;
}

static void wifi_manager_task(void *arg) {
  (void)arg;
  uint8_t retries = 0;
  bool connected = false;
  for (;;) {
    EventBits_t bits =
        xEventGroupWaitBits(s_wifi_events,
                            WIFI_GOT_IP_BIT | WIFI_DISCONNECTED_BIT, pdTRUE,
                            pdFALSE, pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));
    if ((bits & WIFI_GOT_IP_BIT) != 0) {
      connected = true;
      retries = 0;
      esp_err_t err = start_http_server();
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "Configuration server unavailable after Wi-Fi connect");
      }
      continue;
    }

    if (connected && bits == 0) {
      continue;
    }
    connected = false;
    if (retries < WIFI_RETRY_LIMIT) {
      retries++;
      ESP_LOGW(TAG, "Wi-Fi connection attempt %u/%u", retries,
               WIFI_RETRY_LIMIT);
      vTaskDelay(pdMS_TO_TICKS(500));
      esp_err_t err = esp_wifi_connect();
      if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi reconnect request failed: %s",
                 esp_err_to_name(err));
      }
      continue;
    }

    ESP_LOGW(TAG, "Wi-Fi unavailable; switching to setup AP only");
    esp_err_t err = esp_wifi_stop();
    if (err == ESP_OK) {
      s_wifi_started = false;
      err = configure_setup_ap();
    }
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Could not start fallback setup AP: %s",
               esp_err_to_name(err));
    }
    break;
  }
  vTaskDelete(NULL);
}

esp_err_t wifi_portal_start(const app_config_t *config) {
  if (config == NULL || !app_config_is_valid(config)) {
    return ESP_ERR_INVALID_ARG;
  }
  s_config = *config;

  esp_err_t err = esp_netif_init();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    return err;
  }
  err = esp_event_loop_create_default();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    return err;
  }

  s_ap_netif = esp_netif_create_default_wifi_ap();
  s_sta_netif = esp_netif_create_default_wifi_sta();
  if (s_ap_netif == NULL || s_sta_netif == NULL) {
    return ESP_ERR_NO_MEM;
  }

  wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
  err = esp_wifi_init(&wifi_config);
  if (err != ESP_OK) {
    return err;
  }
  s_wifi_events = xEventGroupCreate();
  if (s_wifi_events == NULL) {
    return ESP_ERR_NO_MEM;
  }
  err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                   wifi_event_handler, NULL);
  if (err == ESP_OK) {
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                     wifi_event_handler, NULL);
  }
  if (err != ESP_OK) {
    return err;
  }

  if (s_config.wifi_ssid[0] == '\0') {
    return configure_setup_ap();
  }

  wifi_config_t station_config = {0};
  size_t ssid_length = strnlen(s_config.wifi_ssid, sizeof(s_config.wifi_ssid));
  size_t password_length =
      strnlen(s_config.wifi_password, sizeof(s_config.wifi_password));
  memcpy(station_config.sta.ssid, s_config.wifi_ssid, ssid_length);
  memcpy(station_config.sta.password, s_config.wifi_password,
         password_length);
  station_config.sta.threshold.authmode =
      password_length == 0 ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

  err = esp_wifi_set_mode(WIFI_MODE_STA);
  if (err == ESP_OK) {
    err = esp_wifi_set_config(WIFI_IF_STA, &station_config);
  }
  if (err == ESP_OK) {
    err = esp_wifi_start();
  }
  if (err == ESP_OK) {
    s_wifi_started = true;
    err = esp_wifi_connect();
  }
  if (err != ESP_OK && !s_wifi_started) {
    return err;
  }
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Initial Wi-Fi connection request failed: %s",
             esp_err_to_name(err));
  }

  BaseType_t created =
      xTaskCreate(wifi_manager_task, "wifi_manager", 4096, NULL, 4, NULL);
  if (created != pdPASS) {
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}
