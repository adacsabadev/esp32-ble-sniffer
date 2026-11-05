#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "led_strip.h"
#include "sdkconfig.h"
#include "status_led.h"

static const char *TAG = "LED";

static led_strip_handle_t s_strip;
static TaskHandle_t s_task;
static volatile unsigned s_connections;
static volatile bool s_activity;

#define BLINK_OFF_MS 40
#define BLINK_HOLD_MS 40

static void led_show(bool on)
{
    if (on) {
        led_strip_set_pixel(s_strip, 0, 0, 0, CONFIG_DONGLE_LED_BRIGHTNESS);
        led_strip_refresh(s_strip);
    } else {
        led_strip_clear(s_strip);
    }
}

static void led_task(void *arg)
{
    bool on = false;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        bool want = s_connections > 0;
        if (want != on) {
            on = want;
            led_show(on);
            ESP_LOGI(TAG, "RGB LED %s", on ? "blue (BLE device connected)" : "off (no BLE connection)");
        }
        if (on && s_activity) {
            /* blink: dip off, restore, then ignore traffic during the hold so it stays a visible blink */
            led_show(false);
            vTaskDelay(pdMS_TO_TICKS(BLINK_OFF_MS));
            led_show(s_connections > 0);
            vTaskDelay(pdMS_TO_TICKS(BLINK_HOLD_MS));
            s_activity = false;
            xTaskNotifyStateClear(NULL);
            on = s_connections > 0;
            led_show(on);
        }
    }
}

void status_led_activity(void)
{
    if (s_task && s_connections > 0 && !s_activity) {
        s_activity = true;
        xTaskNotifyGive(s_task);
    }
}

void status_led_set_connections(unsigned count)
{
    s_connections = count;
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

esp_err_t status_led_start(void)
{
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = CONFIG_DONGLE_LED_GPIO,
        .max_leds = 1,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .resolution_hz = 10 * 1000 * 1000,
    };
    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LED init on GPIO%d failed: %s", CONFIG_DONGLE_LED_GPIO, esp_err_to_name(err));
        return err;
    }
    led_strip_clear(s_strip);
    xTaskCreatePinnedToCore(led_task, "led", 2048, NULL, 1, &s_task, 1);
    ESP_LOGI(TAG, "status LED on GPIO%d, blue level %d", CONFIG_DONGLE_LED_GPIO, CONFIG_DONGLE_LED_BRIGHTNESS);
    return ESP_OK;
}
