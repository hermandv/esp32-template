/*
 * Template test module (SPEC §5.8). It shows the pattern every module follows:
 *   1. register its stream channels once,
 *   2. measure only while someone listens (devlink_channel_wanted_hz),
 *   3. publish with devlink_publish_*, debug text with devlink_debug.
 * Module rules (§5.8): no GPIO19/20, no button/LED pins, no deep sleep without a wake-up (use devlink_sleep), never
 * write factory/dlcfg/dlboot/otadata.
 *
 * The data here is synthetic: example.sine (a 0.5 Hz sine) and example.adc (three noisy "ADC" values).
 */
#include "mod_example.h"

#include <math.h>
#include <stdlib.h>

#include "devlink_stream.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "mod_example";

static devlink_channel_t *s_sine;
static devlink_channel_t *s_adc;

static void example_task(void *arg)
{
    (void)arg;
    uint32_t n = 0;
    for (;;) {
        uint32_t hz_sine = devlink_channel_wanted_hz(s_sine);
        uint32_t hz_adc = devlink_channel_wanted_hz(s_adc);
        uint32_t hz = (hz_sine > hz_adc) ? hz_sine : hz_adc;
        if (hz == 0u) {
            vTaskDelay(pdMS_TO_TICKS(100)); /* nobody listens: measure nothing */
            continue;
        }
        double t = (double)esp_timer_get_time() / 1e6;
        if (hz_sine > 0u) {
            devlink_publish_num(s_sine, sin(2.0 * M_PI * 0.5 * t)); /* devlink applies each client's rate */
        }
        if (hz_adc > 0u) {
            double v[3] = {1000.0 + (rand() % 50), 2000.0 + (rand() % 50), 3000.0 + (rand() % 50)};
            devlink_publish_arr(s_adc, v, 3);
        }
        if ((++n % (hz * 10u)) == 0u) {
            devlink_debug("example", "running, %u samples so far", (unsigned)n);
        }
        TickType_t d = pdMS_TO_TICKS(1000u / hz);
        vTaskDelay((d > 0u) ? d : 1u);
    }
}

esp_err_t mod_example_start(void)
{
    s_sine = devlink_channel_register(
        &(devlink_channel_cfg_t){.name = "example.sine", .unit = "", .type = DL_NUM, .default_hz = 10, .max_hz = 200});
    s_adc = devlink_channel_register(
        &(devlink_channel_cfg_t){.name = "example.adc", .unit = "counts", .type = DL_ARR, .default_hz = 5,
                                 .max_hz = 50});
    if ((s_sine == NULL) || (s_adc == NULL)) {
        ESP_LOGE(TAG, "channel registration failed");
        return ESP_FAIL;
    }
    return (xTaskCreate(example_task, "mod_example", 3072, NULL, 3, NULL) == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}
