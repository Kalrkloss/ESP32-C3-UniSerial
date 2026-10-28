#include "telnet_bridge.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>

#include "bridge_status_led.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#define TELNET_TASK_STACK_SIZE 6144
#define TELNET_TASK_PRIORITY 5
#define TELNET_POLL_TIMEOUT_MS 20
#define TELNET_SEND_DEADLINE_US 100000
#define TELNET_LISTEN_BACKLOG 1
#define TELNET_IAC 255
#define TELNET_DONT 254
#define TELNET_DO 253
#define TELNET_WONT 252
#define TELNET_WILL 251
#define TELNET_SB 250
#define TELNET_SE 240

static const char *TAG = "telnet_bridge";

typedef enum {
  IAC_DATA,
  IAC_COMMAND,
  IAC_OPTION,
  IAC_SUBNEGOTIATION,
  IAC_SUBNEGOTIATION_COMMAND,
} telnet_iac_state_t;

typedef enum {
  AUTH_USERNAME,
  AUTH_PASSWORD,
  AUTH_DATA,
} telnet_auth_state_t;

typedef struct {
  int client_socket;
  telnet_iac_state_t iac_state;
  telnet_auth_state_t auth_state;
  uint8_t negotiation_command;
  char username[APP_TELNET_USERNAME_MAX + 1];
  char line[APP_TELNET_PASSWORD_MAX + 1];
  size_t line_length;
  unsigned int failed_attempts;
  bool username_invalid;
  bool line_invalid;
  bool skip_line_feed;
} telnet_session_t;

typedef struct {
  const app_config_t *config;
  uart_port_t uart_port;
  QueueHandle_t uart_queue;
  int listener_socket;
  TaskHandle_t server_task;
} telnet_task_args_t;

typedef struct {
  const telnet_task_args_t *args;
  telnet_session_t *session;
} telnet_uart_rx_task_args_t;

static telnet_task_args_t s_task_args;
static bool s_started;
static SemaphoreHandle_t s_client_mutex;
static uint8_t s_uart_tx_buffer[APP_UART_BUFFER_MAX];
static uint8_t s_uart_rx_buffer[APP_UART_BUFFER_MAX];
static uint8_t s_encoded_buffer[APP_UART_BUFFER_MAX * 2];

static bool set_nonblocking(int socket_fd) {
  int flags = fcntl(socket_fd, F_GETFL, 0);
  return flags >= 0 && fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static bool send_all(int socket_fd, const uint8_t *data, size_t length) {
  size_t sent = 0;
  int64_t deadline = esp_timer_get_time() + TELNET_SEND_DEADLINE_US;

  while (sent < length) {
    if (esp_timer_get_time() >= deadline) {
      errno = ETIMEDOUT;
      break;
    }
    int result = send(socket_fd, data + sent, length - sent, 0);
    if (result > 0) {
      sent += (size_t)result;
      continue;
    }
    if (result == 0) {
      errno = ECONNRESET;
      break;
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
      break;
    }

    int64_t remaining = deadline - esp_timer_get_time();
    if (remaining <= 0) {
      errno = ETIMEDOUT;
      break;
    }
    fd_set write_fds;
    FD_ZERO(&write_fds);
    FD_SET(socket_fd, &write_fds);
    struct timeval timeout = {
        .tv_sec = (time_t)(remaining / 1000000),
        .tv_usec = (suseconds_t)(remaining % 1000000),
    };
    result = select(socket_fd + 1, NULL, &write_fds, NULL, &timeout);
    if (result < 0 && errno == EINTR) {
      continue;
    }
    if (result <= 0) {
      if (result == 0) {
        errno = ETIMEDOUT;
      }
      break;
    }
  }

  if (sent != length) {
    ESP_LOGW(TAG, "Telnet socket send failed: errno=%d", errno);
    return false;
  }
  return true;
}

static bool send_text(int socket_fd, const char *text) {
  return send_all(socket_fd, (const uint8_t *)text, strlen(text));
}

static bool send_telnet_data(int socket_fd, const uint8_t *data,
                             size_t length) {
  size_t encoded_length = 0;
  for (size_t i = 0; i < length; i++) {
    s_encoded_buffer[encoded_length++] = data[i];
    if (data[i] == TELNET_IAC) {
      s_encoded_buffer[encoded_length++] = TELNET_IAC;
    }
  }
  return send_all(socket_fd, s_encoded_buffer, encoded_length);
}

static bool send_negotiation_reply(int socket_fd, uint8_t command,
                                   uint8_t option) {
  const uint8_t reply[] = {TELNET_IAC, command, option};
  return send_all(socket_fd, reply, sizeof(reply));
}

static void reset_session(telnet_session_t *session) {
  memset(session, 0, sizeof(*session));
  session->client_socket = -1;
}

static void close_client(telnet_session_t *session) {
  xSemaphoreTake(s_client_mutex, portMAX_DELAY);
  if (session->client_socket >= 0) {
    shutdown(session->client_socket, 0);
    close(session->client_socket);
  }
  reset_session(session);
  xSemaphoreGive(s_client_mutex);
  bridge_status_led_set_telnet_connected(false);
}

static void discard_queued_uart_input(const telnet_task_args_t *args) {
  esp_err_t err = uart_flush_input(args->uart_port);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Could not flush UART before client session: %s",
             esp_err_to_name(err));
  }
  if (xQueueReset(args->uart_queue) != pdPASS) {
    ESP_LOGW(TAG, "Could not reset UART event queue before client session");
  }
}

static bool send_auth_prompt(telnet_session_t *session) {
  return send_text(session->client_socket, "Username: ");
}

static bool handle_auth_line(const telnet_task_args_t *args,
                             telnet_session_t *session) {
  xSemaphoreTake(s_client_mutex, portMAX_DELAY);
  session->line[session->line_length] = '\0';
  if (session->auth_state == AUTH_USERNAME) {
    session->username_invalid = session->line_invalid;
    if (!session->username_invalid) {
      memcpy(session->username, session->line, session->line_length + 1);
    }
    session->auth_state = AUTH_PASSWORD;
    session->line_length = 0;
    session->line_invalid = false;
    memset(session->line, 0, sizeof(session->line));
    bool sent = send_text(session->client_socket, "Password: ");
    xSemaphoreGive(s_client_mutex);
    return sent;
  }

  bool authenticated =
      !session->username_invalid && !session->line_invalid &&
      strcmp(session->username, args->config->telnet_username) == 0 &&
      strcmp(session->line, args->config->telnet_password) == 0;
  memset(session->line, 0, sizeof(session->line));
  session->line_length = 0;
  session->line_invalid = false;

  if (authenticated) {
    session->auth_state = AUTH_DATA;
    memset(session->username, 0, sizeof(session->username));
    bool sent = send_text(session->client_socket, "Login successful\r\n");
    xSemaphoreGive(s_client_mutex);
    return sent;
  }

  session->failed_attempts++;
  if (!send_text(session->client_socket, "Login incorrect\r\n")) {
    xSemaphoreGive(s_client_mutex);
    return false;
  }
  if (session->failed_attempts >= 3) {
    xSemaphoreGive(s_client_mutex);
    return false;
  }

  memset(session->username, 0, sizeof(session->username));
  session->username_invalid = false;
  session->auth_state = AUTH_USERNAME;
  bool sent = send_auth_prompt(session);
  xSemaphoreGive(s_client_mutex);
  return sent;
}

static bool handle_auth_byte(const telnet_task_args_t *args,
                             telnet_session_t *session, uint8_t byte) {
  if (byte == '\r' || byte == '\n') {
    session->skip_line_feed = byte == '\r';
    return handle_auth_line(args, session);
  }
  if (byte == 0 || byte < 0x20 || byte == 0x7f) {
    session->line_invalid = true;
    return true;
  }

  size_t limit = session->auth_state == AUTH_USERNAME
                     ? APP_TELNET_USERNAME_MAX
                     : APP_TELNET_PASSWORD_MAX;
  if (session->line_length >= limit) {
    session->line_invalid = true;
    return true;
  }
  session->line[session->line_length++] = (char)byte;
  return true;
}

static bool handle_application_byte(const telnet_task_args_t *args,
                                    telnet_session_t *session, uint8_t byte,
                                    size_t *uart_length) {
  if (session->skip_line_feed) {
    session->skip_line_feed = false;
    if (byte == '\n') {
      return true;
    }
  }
  if (session->auth_state == AUTH_DATA) {
    s_uart_tx_buffer[(*uart_length)++] = byte;
    return true;
  }
  return handle_auth_byte(args, session, byte);
}

static bool write_uart_data(const telnet_task_args_t *args,
                            const uint8_t *data, size_t length) {
  size_t written_total = 0;
  while (written_total < length) {
    int written = uart_write_bytes(
        args->uart_port, (const char *)data + written_total,
        length - written_total);
    if (written <= 0) {
      ESP_LOGE(TAG, "UART write made no progress: %d", written);
      return false;
    }
    written_total += (size_t)written;
  }
  bridge_status_led_activity();
  return true;
}

static bool process_client_data(const telnet_task_args_t *args,
                                telnet_session_t *session,
                                const uint8_t *data, size_t length) {
  size_t uart_length = 0;
  for (size_t i = 0; i < length; i++) {
    uint8_t byte = data[i];
    switch (session->iac_state) {
    case IAC_DATA:
      if (byte == TELNET_IAC) {
        session->iac_state = IAC_COMMAND;
      } else if (!handle_application_byte(args, session, byte, &uart_length)) {
        return false;
      }
      break;
    case IAC_COMMAND:
      if (byte == TELNET_IAC) {
        session->iac_state = IAC_DATA;
        if (!handle_application_byte(args, session, TELNET_IAC,
                                     &uart_length)) {
          return false;
        }
      } else if (byte == TELNET_WILL || byte == TELNET_WONT ||
                 byte == TELNET_DO || byte == TELNET_DONT) {
        session->negotiation_command = byte;
        session->iac_state = IAC_OPTION;
      } else if (byte == TELNET_SB) {
        session->iac_state = IAC_SUBNEGOTIATION;
      } else {
        session->iac_state = IAC_DATA;
      }
      break;
    case IAC_OPTION:
      if (session->negotiation_command == TELNET_WILL) {
        xSemaphoreTake(s_client_mutex, portMAX_DELAY);
        bool sent = send_negotiation_reply(
            session->client_socket, TELNET_DONT, byte);
        xSemaphoreGive(s_client_mutex);
        if (!sent) {
          return false;
        }
      } else if (session->negotiation_command == TELNET_DO) {
        xSemaphoreTake(s_client_mutex, portMAX_DELAY);
        bool sent = send_negotiation_reply(
            session->client_socket, TELNET_WONT, byte);
        xSemaphoreGive(s_client_mutex);
        if (!sent) {
          return false;
        }
      }
      session->iac_state = IAC_DATA;
      break;
    case IAC_SUBNEGOTIATION:
      if (byte == TELNET_IAC) {
        session->iac_state = IAC_SUBNEGOTIATION_COMMAND;
      }
      break;
    case IAC_SUBNEGOTIATION_COMMAND:
      session->iac_state = byte == TELNET_SE ? IAC_DATA
                                             : IAC_SUBNEGOTIATION;
      break;
    }
  }

  return uart_length == 0 ||
         write_uart_data(args, s_uart_tx_buffer, uart_length);
}

static void handle_uart_event(const telnet_task_args_t *args,
                              telnet_session_t *session,
                              const uart_event_t *event) {
  if (event->type == UART_DATA) {
    bool send_failed = false;
    xSemaphoreTake(s_client_mutex, portMAX_DELAY);
    size_t remaining = event->size;
    while (remaining > 0) {
      size_t chunk = remaining > sizeof(s_uart_rx_buffer)
                         ? sizeof(s_uart_rx_buffer)
                         : remaining;
      int received =
          uart_read_bytes(args->uart_port, s_uart_rx_buffer, chunk, 0);
      if (received <= 0) {
        ESP_LOGW(TAG, "UART RX event had no readable bytes: %d", received);
        break;
      }
      bridge_status_led_activity();
      remaining -= (size_t)received;
      if (session->client_socket >= 0 &&
          session->auth_state == AUTH_DATA &&
          !send_telnet_data(session->client_socket, s_uart_rx_buffer,
                            (size_t)received)) {
        ESP_LOGW(TAG, "Closing slow or failed Telnet client");
        send_failed = true;
        break;
      }
    }
    xSemaphoreGive(s_client_mutex);
    if (send_failed) {
      xTaskNotifyGive(args->server_task);
    }
  } else if (event->type == UART_FIFO_OVF ||
             event->type == UART_BUFFER_FULL) {
    ESP_LOGE(TAG, "UART RX buffer overflow");
    xSemaphoreTake(s_client_mutex, portMAX_DELAY);
    esp_err_t err = uart_flush_input(args->uart_port);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "UART input flush failed: %s", esp_err_to_name(err));
    }
    if (xQueueReset(args->uart_queue) != pdPASS) {
      ESP_LOGE(TAG, "UART event queue reset failed");
    }
    xSemaphoreGive(s_client_mutex);
  } else if (event->type == UART_PARITY_ERR ||
             event->type == UART_FRAME_ERR) {
    ESP_LOGW(TAG, "UART receive error: event=%d", event->type);
  }
}

static void telnet_uart_rx_task(void *arg) {
  const telnet_uart_rx_task_args_t *task_args = arg;
  for (;;) {
    uart_event_t event;
    if (xQueueReceive(task_args->args->uart_queue, &event, portMAX_DELAY) ==
        pdTRUE) {
      handle_uart_event(task_args->args, task_args->session, &event);
    }
  }
}

static void accept_client(const telnet_task_args_t *args,
                          telnet_session_t *session) {
  struct sockaddr_in peer_address;
  socklen_t peer_length = sizeof(peer_address);
  int client_socket =
      accept(args->listener_socket, (struct sockaddr *)&peer_address,
             &peer_length);
  if (client_socket < 0) {
    if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
      ESP_LOGW(TAG, "Telnet accept failed: errno=%d", errno);
    }
    return;
  }

  if (session->client_socket >= 0) {
    ESP_LOGW(TAG, "Rejecting additional Telnet client");
    shutdown(client_socket, 0);
    close(client_socket);
    return;
  }
  if (client_socket >= FD_SETSIZE) {
    ESP_LOGE(TAG, "Telnet client socket exceeds select descriptor limit");
    close(client_socket);
    return;
  }
  if (!set_nonblocking(client_socket)) {
    ESP_LOGE(TAG, "Could not configure Telnet client socket: errno=%d", errno);
    close(client_socket);
    return;
  }

  xSemaphoreTake(s_client_mutex, portMAX_DELAY);
  discard_queued_uart_input(args);
  reset_session(session);
  session->client_socket = client_socket;
  if (args->config->telnet_password[0] != '\0') {
    session->auth_state = AUTH_USERNAME;
    if (!send_auth_prompt(session)) {
      shutdown(session->client_socket, 0);
      close(session->client_socket);
      reset_session(session);
      xSemaphoreGive(s_client_mutex);
      return;
    }
  } else {
    session->auth_state = AUTH_DATA;
  }
  xSemaphoreGive(s_client_mutex);
  bridge_status_led_set_telnet_connected(true);
  ESP_LOGI(TAG, "Telnet client connected");
}

static void telnet_server_task(void *arg) {
  const telnet_task_args_t *args = arg;
  telnet_session_t session;
  reset_session(&session);
  s_task_args.server_task = xTaskGetCurrentTaskHandle();
  telnet_uart_rx_task_args_t uart_rx_args = {
      .args = args,
      .session = &session,
  };
  TaskHandle_t uart_rx_task = NULL;
  if (xTaskCreate(telnet_uart_rx_task, "telnet_uart_rx",
                  TELNET_TASK_STACK_SIZE, &uart_rx_args,
                  TELNET_TASK_PRIORITY, &uart_rx_task) != pdPASS) {
    ESP_LOGE(TAG, "Failed to create Telnet UART RX task");
    shutdown(args->listener_socket, 0);
    close(args->listener_socket);
    vTaskDelete(NULL);
    return;
  }
  uint8_t socket_buffer[APP_UART_BUFFER_MAX];

  ESP_LOGI(TAG, "Telnet server listening on port %u",
           args->config->telnet_port);
  for (;;) {
    if (ulTaskNotifyTake(pdTRUE, 0) > 0) {
      ESP_LOGW(TAG, "Closing Telnet client after UART-to-socket failure");
      close_client(&session);
    }

    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(args->listener_socket, &read_fds);
    int max_fd = args->listener_socket;
    if (session.client_socket >= 0) {
      FD_SET(session.client_socket, &read_fds);
      if (session.client_socket > max_fd) {
        max_fd = session.client_socket;
      }
    }

    struct timeval timeout = {
        .tv_sec = 0,
        .tv_usec = TELNET_POLL_TIMEOUT_MS * 1000,
    };
    int ready = select(max_fd + 1, &read_fds, NULL, NULL, &timeout);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      ESP_LOGE(TAG, "Telnet socket select failed: errno=%d", errno);
      close_client(&session);
      break;
    }

    if (FD_ISSET(args->listener_socket, &read_fds)) {
      accept_client(args, &session);
    }
    if (session.client_socket >= 0 &&
        FD_ISSET(session.client_socket, &read_fds)) {
      int received =
          recv(session.client_socket, socket_buffer, sizeof(socket_buffer), 0);
      if (received > 0) {
        if (!process_client_data(args, &session, socket_buffer,
                                 (size_t)received)) {
          ESP_LOGW(TAG, "Closing Telnet client after data-path error");
          close_client(&session);
        }
      } else if (received == 0 ||
                 (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
        if (received < 0) {
          ESP_LOGW(TAG, "Telnet client receive failed: errno=%d", errno);
        }
        close_client(&session);
      }
    }
  }

  vTaskDelete(uart_rx_task);
  shutdown(args->listener_socket, 0);
  close(args->listener_socket);
  ESP_LOGE(TAG, "Telnet listener stopped");
  vTaskDelete(NULL);
}

esp_err_t telnet_bridge_start(const app_config_t *config, uart_port_t uart_port,
                              QueueHandle_t uart_queue) {
  if (config == NULL || uart_queue == NULL || !config->telnet_enabled ||
      config->telnet_port == 0 || config->telnet_port == 80) {
    return ESP_ERR_INVALID_ARG;
  }
  if (s_started) {
    return ESP_ERR_INVALID_STATE;
  }

  int listener_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
  if (listener_socket < 0) {
    ESP_LOGE(TAG, "Telnet socket creation failed: errno=%d", errno);
    return ESP_FAIL;
  }
  int reuse_address = 1;
  if (setsockopt(listener_socket, SOL_SOCKET, SO_REUSEADDR, &reuse_address,
                 sizeof(reuse_address)) < 0) {
    ESP_LOGE(TAG, "Telnet socket option setup failed: errno=%d", errno);
    close(listener_socket);
    return ESP_FAIL;
  }
  if (listener_socket >= FD_SETSIZE) {
    ESP_LOGE(TAG, "Telnet listener exceeds select descriptor limit");
    close(listener_socket);
    return ESP_FAIL;
  }
  if (!set_nonblocking(listener_socket)) {
    ESP_LOGE(TAG, "Could not configure Telnet listener socket: errno=%d",
             errno);
    close(listener_socket);
    return ESP_FAIL;
  }

  struct sockaddr_in address = {
      .sin_family = AF_INET,
      .sin_port = htons(config->telnet_port),
      .sin_addr.s_addr = htonl(INADDR_ANY),
  };
  if (bind(listener_socket, (struct sockaddr *)&address, sizeof(address)) < 0) {
    ESP_LOGE(TAG, "Telnet bind on port %u failed: errno=%d",
             config->telnet_port, errno);
    close(listener_socket);
    return ESP_FAIL;
  }
  if (listen(listener_socket, TELNET_LISTEN_BACKLOG) < 0) {
    ESP_LOGE(TAG, "Telnet listen failed: errno=%d", errno);
    close(listener_socket);
    return ESP_FAIL;
  }

  s_task_args = (telnet_task_args_t){
      .config = config,
      .uart_port = uart_port,
      .uart_queue = uart_queue,
      .listener_socket = listener_socket,
  };
  s_client_mutex = xSemaphoreCreateMutex();
  if (s_client_mutex == NULL) {
    ESP_LOGE(TAG, "Telnet client mutex creation failed");
    close(listener_socket);
    return ESP_ERR_NO_MEM;
  }
  if (xTaskCreate(telnet_server_task, "telnet_bridge",
                  TELNET_TASK_STACK_SIZE, &s_task_args, TELNET_TASK_PRIORITY,
                  NULL) != pdPASS) {
    ESP_LOGE(TAG, "Telnet server task creation failed");
    vSemaphoreDelete(s_client_mutex);
    s_client_mutex = NULL;
    close(listener_socket);
    return ESP_ERR_NO_MEM;
  }
  s_started = true;
  return ESP_OK;
}
