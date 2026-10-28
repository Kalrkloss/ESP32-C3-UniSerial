/*
 *    esp32_uart_bridge.c
 *
 *     ________  ________  ___  ________  ________  _______
 *    |\   __  \|\   __  \|\  \|\   ___ \|\   ____\|\  ___ \
 *    \ \  \|\ /\ \  \|\  \ \  \ \  \_|\ \ \  \___|\ \   __/|
 *     \ \   __  \ \   _  _\ \  \ \  \ \\ \ \  \  __\ \  \_|/__
 *      \ \  \|\  \ \  \\  \\ \  \ \  \_\\ \ \  \|\  \ \  \_|\ \
 *       \ \_______\ \__\\ _\\ \__\ \_______\ \_______\ \_______\
 *        \|_______|\|__|\|__|\|__|\|_______|\|_______|\|_______|
 *
 *
 *    Copyright (c) 2023 Alien Green LLC
 *
 *    This file is part of UnitVM.
 *
 *    UnitVM is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    UnitVM is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with UnitVM.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    ASCII font see http://patorjk.com/software/taag/#p=display&f=3D-ASCII
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>

#include "app_config.h"
#include "bridge_status_led.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "hal/usb_serial_jtag_ll.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "telnet_bridge.h"
#include "wifi_portal.h"

#define UBRIDGE_PIN_TXD CONFIG_UBRIDGE_UART_TXD
#define UBRIDGE_PIN_RXD CONFIG_UBRIDGE_UART_RXD
#define UBRIDGE_PIN_RTS UART_PIN_NO_CHANGE
#define UBRIDGE_PIN_CTS UART_PIN_NO_CHANGE

#define UBRIDGE_UART_PORT_NUM CONFIG_UBRIDGE_UART_PORT_NUM
#define UBRIDGE_UART_BAUD_RATE CONFIG_UBRIDGE_UART_BAUD_RATE
#define UBRIDGE_TASK_STACK_SIZE CONFIG_UBRIDGE_TASK_STACK_SIZE

#define BUF_SIZE APP_UART_BUFFER_MAX
#define BRIDGE_INIT_TASK_STACK_SIZE 4096

static const char *TAG = "uart_bridge";
static uint8_t usb_to_uart_buffer[BUF_SIZE];
static uint8_t uart_to_usb_buffer[BUF_SIZE];
static QueueHandle_t uart_queue;
static app_config_t bridge_config;

#define DVAL2(x) #x
#define DVAL(x) DVAL2(x)
#define DO_PRAGMA(x) _Pragma(#x)
#define PRINT_DIRECTIVE(x) DO_PRAGMA(message(#x " = " DVAL(x)))
PRINT_DIRECTIVE(CONFIG_UBRIDGE_UART_PARITY)

#define GPIO_OUTPUT_IO_NRST 7
#define GPIO_OUTPUT_IO_BOOT0 8
#define GPIO_OUTPUT_PIN_SEL                                                    \
  ((1 << GPIO_OUTPUT_IO_NRST) | (1 << GPIO_OUTPUT_IO_BOOT0))

/* ----------------------------------------------------------- */

void dfu_mode() {
  gpio_set_level(GPIO_OUTPUT_IO_BOOT0, 1); // BOOT0 = HIGH
  gpio_set_level(GPIO_OUTPUT_IO_NRST, 0);  // NRST = LOW
  vTaskDelay(20 / portTICK_PERIOD_MS);
  gpio_set_level(GPIO_OUTPUT_IO_NRST, 1); // NRST = HIGH
}

/* ----------------------------------------------------------- */

void run_mode() {
  gpio_set_level(GPIO_OUTPUT_IO_BOOT0, 0); // BOOT0 = LOW
  gpio_set_level(GPIO_OUTPUT_IO_NRST, 0);  // NRST = LOW
  vTaskDelay(20 / portTICK_PERIOD_MS);
  gpio_set_level(GPIO_OUTPUT_IO_NRST, 1); // NRST = HIGH
}

/* ----------------------------------------------------------- */

static void usb_to_uart_task(void *arg) {
  while (true) {
    int len = usb_serial_jtag_read_bytes(
        usb_to_uart_buffer, bridge_config.uart_buffer_size, portMAX_DELAY);
    if (len > 0) {
      int written = uart_write_bytes(UBRIDGE_UART_PORT_NUM,
                                     (const char *)usb_to_uart_buffer, len);
      if (written > 0) {
        bridge_status_led_activity();
      }
      if (written != len) {
        ESP_LOGE(TAG, "UART write failed: %d of %d bytes", written, len);
      }
    }
  }
}

static void uart_to_usb_task(void *arg) {
  uart_event_t event;

  while (true) {
    if (xQueueReceive(uart_queue, &event, portMAX_DELAY) != pdTRUE) {
      continue;
    }

    if (event.type == UART_DATA) {
      size_t remaining = event.size;
      while (remaining > 0) {
        uint32_t chunk_size =
            remaining > bridge_config.uart_buffer_size
                ? bridge_config.uart_buffer_size
                : (uint32_t)remaining;
        int len = uart_read_bytes(UBRIDGE_UART_PORT_NUM, uart_to_usb_buffer,
                                  chunk_size, 0);
        if (len <= 0) {
          ESP_LOGE(TAG, "UART RX buffer read failed: %d", len);
          break;
        }

        bridge_status_led_activity();
        int written = usb_serial_jtag_write_bytes(uart_to_usb_buffer, len,
                                                  portMAX_DELAY);
        if (written != len) {
          ESP_LOGE(TAG, "USB write failed: %d of %d bytes", written, len);
        }
        usb_serial_jtag_ll_txfifo_flush();
        remaining -= len;
      }
    } else if (event.type == UART_FIFO_OVF ||
               event.type == UART_BUFFER_FULL) {
      ESP_LOGE(TAG, "UART RX buffer overflow");
      ESP_ERROR_CHECK(uart_flush_input(UBRIDGE_UART_PORT_NUM));
      xQueueReset(uart_queue);
    } else if (event.type == UART_PARITY_ERR || event.type == UART_FRAME_ERR) {
      ESP_LOGW(TAG, "UART receive error: event=%d", event.type);
    }
  }
}

/* ----------------------------------------------------------- */

static uart_word_length_t get_uart_data_bits(uint8_t data_bits) {
  switch (data_bits) {
  case 5:
    return UART_DATA_5_BITS;
  case 6:
    return UART_DATA_6_BITS;
  case 7:
    return UART_DATA_7_BITS;
  default:
    return UART_DATA_8_BITS;
  }
}

static uart_parity_t get_uart_parity(app_uart_parity_t parity) {
  switch (parity) {
  case APP_UART_PARITY_EVEN:
    return UART_PARITY_EVEN;
  case APP_UART_PARITY_ODD:
    return UART_PARITY_ODD;
  default:
    return UART_PARITY_DISABLE;
  }
}

static uart_stop_bits_t get_uart_stop_bits(uint8_t stop_bits) {
  switch (stop_bits) {
  case 15:
    return UART_STOP_BITS_1_5;
  case 2:
    return UART_STOP_BITS_2;
  default:
    return UART_STOP_BITS_1;
  }
}

static void init_nvs_and_config(void) {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
      err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_LOGW(TAG, "NVS needs recovery; stored configuration will be erased");
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(err);
  ESP_ERROR_CHECK(app_config_load(&bridge_config));
}

static void bridge_task(void *arg) {

  gpio_config_t io_conf;
  io_conf.intr_type = GPIO_INTR_DISABLE;
  io_conf.mode = GPIO_MODE_OUTPUT;
  io_conf.pin_bit_mask = GPIO_OUTPUT_PIN_SEL;
  io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
  io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
  ESP_ERROR_CHECK(gpio_config(&io_conf));

  /*Run SMT32 */
  dfu_mode();
  ESP_ERROR_CHECK(bridge_status_led_start(!bridge_config.telnet_enabled));

  if (!bridge_config.telnet_enabled) {
    /* Configure USB-CDC */
    usb_serial_jtag_driver_config_t usb_serial_config = {
        .tx_buffer_size = bridge_config.uart_buffer_size,
        .rx_buffer_size = bridge_config.uart_buffer_size,
    };
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb_serial_config));
  }

  /* Configure parameters of an UART driver,
   * communication pins and install the driver */
  uart_config_t uart_config = {
      .baud_rate = (int)bridge_config.uart_baud_rate,
      .data_bits = get_uart_data_bits(bridge_config.uart_data_bits),
      .parity = get_uart_parity(bridge_config.uart_parity),
      .stop_bits = get_uart_stop_bits(bridge_config.uart_stop_bits),
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      .source_clk = UART_SCLK_DEFAULT,
  };
  int intr_alloc_flags = 0;

#if CONFIG_UART_ISR_IN_IRAM
  intr_alloc_flags = ESP_INTR_FLAG_IRAM;
#endif

  // gpio_reset_pin(UBRIDGE_PIN_TXD);
  // gpio_reset_pin(UBRIDGE_PIN_RXD);
  // gpio_set_direction(UBRIDGE_PIN_TXD, GPIO_MODE_INPUT);
  // gpio_set_direction(UBRIDGE_PIN_RXD, GPIO_MODE_INPUT);

  ESP_ERROR_CHECK(uart_driver_install(
      UBRIDGE_UART_PORT_NUM, bridge_config.uart_buffer_size * 2, 0, 20,
      &uart_queue, intr_alloc_flags));
  ESP_ERROR_CHECK(uart_param_config(UBRIDGE_UART_PORT_NUM, &uart_config));
  ESP_ERROR_CHECK(uart_set_pin(UBRIDGE_UART_PORT_NUM, UBRIDGE_PIN_TXD,
                               UBRIDGE_PIN_RXD, UBRIDGE_PIN_RTS,
                               UBRIDGE_PIN_CTS));
  uint32_t inverse_mask = 0;
  if (bridge_config.uart_rx_invert) {
    inverse_mask |= UART_SIGNAL_RXD_INV;
  }
  if (bridge_config.uart_tx_invert) {
    inverse_mask |= UART_SIGNAL_TXD_INV;
  }
  ESP_ERROR_CHECK(uart_set_line_inverse(UBRIDGE_UART_PORT_NUM, inverse_mask));

  if (!bridge_config.telnet_enabled) {
    if (xTaskCreate(usb_to_uart_task, "usb_to_uart", UBRIDGE_TASK_STACK_SIZE,
                    NULL, 2 | portPRIVILEGE_BIT, NULL) != pdPASS ||
        xTaskCreate(uart_to_usb_task, "uart_to_usb", UBRIDGE_TASK_STACK_SIZE,
                    NULL, 2 | portPRIVILEGE_BIT, NULL) != pdPASS) {
      ESP_LOGE(TAG, "Failed to create USB bridge tasks");
      abort();
    }
  }

  ESP_ERROR_CHECK(wifi_portal_start(&bridge_config));
  if (bridge_config.telnet_enabled) {
    esp_err_t err =
        telnet_bridge_start(&bridge_config, UBRIDGE_UART_PORT_NUM, uart_queue);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Telnet startup failed; USB bridge remains disabled: %s",
               esp_err_to_name(err));
    }
  }
  vTaskDelete(NULL);
}

/* ----------------------------------------------------------- */

void app_main(void) {

  init_nvs_and_config();
  if (xTaskCreate(bridge_task, "uart_bridge_task",
                  BRIDGE_INIT_TASK_STACK_SIZE, NULL,
                  2 | portPRIVILEGE_BIT, NULL) != pdPASS) {
    ESP_LOGE(TAG, "Failed to create bridge initialization task");
    abort();
  }

#ifdef CONFIG_ESP_TASK_WDT_EN
  /*
   * In case if 'Enable Task Watchdog Timer' is enabled in → Component
   * config → ESP System Settings. We must deinit a WDT on main task to avoid
   * an unexpected system restart.
   */
  esp_task_wdt_deinit();
#endif
}

/* ----------------------------------------------------------- */
