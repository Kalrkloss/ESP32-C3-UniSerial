#include "bridge_status_led.h"

#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define STATUS_LED_GPIO GPIO_NUM_8
#define STATUS_LED_ACTIVITY_US 20000
#define STATUS_LED_ACTIVITY_CYCLE_US 100000
#define STATUS_LED_ACTIVITY_TOGGLE_US 50000
#define STATUS_LED_TASK_PERIOD_MS 5
#define STATUS_LED_TASK_STACK_SIZE 2048

static const char *TAG = "bridge_status_led";
static portMUX_TYPE s_state_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_usb_mode;
static bool s_telnet_connected;
static int64_t s_activity_until_us;
static int64_t s_activity_started_us;

static void status_led_task(void *arg) {
  (void)arg;
  bool led_on = false;

  for (;;) {
    bool connected;
    int64_t activity_until_us;
    if (s_usb_mode) {
      connected = usb_serial_jtag_is_connected();
    } else {
      portENTER_CRITICAL(&s_state_mux);
      connected = s_telnet_connected;
      portEXIT_CRITICAL(&s_state_mux);
    }

    portENTER_CRITICAL(&s_state_mux);
    activity_until_us = s_activity_until_us;
    int64_t activity_started_us = s_activity_started_us;
    portEXIT_CRITICAL(&s_state_mux);

    int64_t now_us = esp_timer_get_time();
    bool activity_active = now_us < activity_until_us;
    bool activity_led_on =
        activity_active &&
        ((now_us - activity_started_us) % STATUS_LED_ACTIVITY_CYCLE_US >=
         STATUS_LED_ACTIVITY_TOGGLE_US);
    bool should_be_on = connected && (!activity_active || activity_led_on);
    if (should_be_on != led_on) {
      ESP_ERROR_CHECK(gpio_set_level(STATUS_LED_GPIO, should_be_on ? 0 : 1));
      led_on = should_be_on;
    }
    vTaskDelay(pdMS_TO_TICKS(STATUS_LED_TASK_PERIOD_MS));
  }
}

esp_err_t bridge_status_led_start(bool usb_mode) {
  gpio_config_t gpio_config_data = {
      .pin_bit_mask = 1ULL << STATUS_LED_GPIO,
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  esp_err_t err = gpio_config(&gpio_config_data);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "GPIO8 configuration failed: %s", esp_err_to_name(err));
    return err;
  }

  err = gpio_set_level(STATUS_LED_GPIO, 1);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Could not turn off status LED: %s", esp_err_to_name(err));
    return err;
  }

  s_usb_mode = usb_mode;
  s_telnet_connected = false;
  s_activity_until_us = 0;
  s_activity_started_us = 0;
  if (xTaskCreate(status_led_task, "bridge_status_led",
                  STATUS_LED_TASK_STACK_SIZE, NULL, 1, NULL) != pdPASS) {
    ESP_LOGE(TAG, "Status LED task creation failed");
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

void bridge_status_led_set_telnet_connected(bool connected) {
  portENTER_CRITICAL(&s_state_mux);
  s_telnet_connected = connected;
  portEXIT_CRITICAL(&s_state_mux);
}

void bridge_status_led_activity(void) {
  int64_t now_us = esp_timer_get_time();
  portENTER_CRITICAL(&s_state_mux);
  if (now_us >= s_activity_until_us) {
    s_activity_started_us = now_us;
  }
  s_activity_until_us = now_us + STATUS_LED_ACTIVITY_US;
  portEXIT_CRITICAL(&s_state_mux);
}
