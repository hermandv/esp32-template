/*
 * App-only parts of devlink (SPEC §3.5, §5.9): the self-test that ends the verify window, and the button.
 */
#include "devlink.h"
#include "dl_button.h"
#include "dl_priv.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "devlink";

#define SELFTEST_POLL_MS 500u
#define HTTP_ALIVE_MS    5000u

/* SPEC §3.5 rule 3: the HTTP API is listening AND the network is usable (STA connected, or the config AP is up and
 * the API serves on it). Wi-Fi settings are not part of the decision (I6): the AP path counts. */
static bool selftest_ok(void)
{
    dl_wifi_status_t st;
    dl_wifi_get_status(&st);
    return (dl_http_heartbeat_age_ms() < HTTP_ALIVE_MS) && (st.sta_connected || st.ap_on);
}

static void selftest_task(void *arg)
{
    (void)arg;
    const int64_t deadline = (int64_t)CONFIG_DEVLINK_SELFTEST_S * 1000000;
    while (esp_timer_get_time() < deadline) {
        if (selftest_ok()) {
            esp_err_t err = devlink_mark_valid(); /* app: valid + RTC WDT off */
            ESP_LOGI(TAG, "self-test passed after %lld ms (%s)", esp_timer_get_time() / 1000, esp_err_to_name(err));
            vTaskDelete(NULL);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(SELFTEST_POLL_MS));
    }
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    (void)esp_ota_get_state_partition(esp_ota_get_running_partition(), &state);
    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        /* SPEC §3.5 rule 5: the new firmware itself is broken -> roll back now. Restarting while still
         * PENDING_VERIFY makes the bootloader do the rollback, so the restored app reports boot_reason=rollback
         * (esp_ota_mark_app_invalid_rollback_and_reboot() would mark it invalid first and the reason would be lost). */
        ESP_LOGE(TAG, "self-test not passed in %d s: rolling back", CONFIG_DEVLINK_SELFTEST_S);
        devlink_restart(); /* planned: the boot log must not count it towards the crash-loop guard */
    }
    /* Already valid (e.g. HTTP broke later in the field): no rollback possible. The RTC WDT stays on, so the board
     * resets after 90 s and, if this repeats, the crash-loop guard takes it to recovery. */
    ESP_LOGE(TAG, "self-test not passed in %d s (app already valid): RTC WDT will reset", CONFIG_DEVLINK_SELFTEST_S);
    dl_note_error("selftest_failed");
    vTaskDelete(NULL);
}

#if CONFIG_DEVLINK_BUTTON_GPIO >= 0
static void button_task(void *arg)
{
    (void)arg;
    const gpio_num_t pin = (gpio_num_t)CONFIG_DEVLINK_BUTTON_GPIO;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    (void)gpio_config(&io);
    dl_button_t b;
    dl_button_init(&b, gpio_get_level(pin) == 0);
    if (b.stuck) {
        ESP_LOGW(TAG, "button GPIO%d reads pressed at boot: ignored until released", (int)pin);
    }
    for (;;) {
        switch (dl_button_step(&b, gpio_get_level(pin) == 0, (uint64_t)(esp_timer_get_time() / 1000))) {
        case DL_BTN_AP:
            ESP_LOGI(TAG, "button 3 s: config AP open");
            dl_wifi_button();
            break;
        case DL_BTN_RECOVERY:
            ESP_LOGI(TAG, "button 10 s: reboot into recovery");
            devlink_restart_to_recovery();
            break;
        default:
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
#endif

esp_err_t dl_app_start(void)
{
    if (xTaskCreate(selftest_task, "dl_selftest", 3072, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
#if CONFIG_DEVLINK_BUTTON_GPIO >= 0
    if (xTaskCreate(button_task, "dl_button", 2560, NULL, 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
#endif
    return ESP_OK;
}
