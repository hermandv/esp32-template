/*
 * Template test module (SPEC §5.8). It shows the pattern every module follows:
 *   1. register its stream channels once,
 *   2. measure only while someone listens (devlink_channel_wanted_hz),
 *   3. publish with devlink_publish_*, debug text with devlink_debug.
 * Module rules (§5.8): no GPIO19/20, no button/LED pins, no deep sleep without a wake-up (use devlink_sleep), never
 * write factory/dlcfg/dlboot/otadata.
 *
 * The data here is synthetic: example.sine (a 0.5 Hz sine) and example.adc (three noisy "ADC" values), plus the
 * block channel example.wave for high rates (SPEC §5.7): int16 samples "sine,count,noise" at up to 1 kHz, in blocks
 * of 50 ms. "count" goes up by one per sample (wrapping), so a client can check that no sample was lost.
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
static devlink_channel_t *s_wave;

#define WAVE_FIELDS 3
#define WAVE_BLOCK_MS 50u
#define WAVE_MAX_SAMPLES 200u /* 200 x 3 x int16 = 1200 bytes <= DEVLINK_BLOCK_MAX_BYTES */

/* A block producer: runs at the rate the subscribers ask for (devlink_channel_wanted_hz) and publishes every
 * WAVE_BLOCK_MS the samples that came due, with the time of the first one and the sample period. */
static void wave_task(void *arg)
{
    (void)arg;
    static int16_t buf[WAVE_MAX_SAMPLES][WAVE_FIELDS];
    static const float scale[WAVE_FIELDS] = {1.0f / 10000.0f, 1.0f, 1.0f}; /* sine in +-1.0 */
    uint32_t hz = 0;
    int64_t t_start = 0;
    uint64_t done = 0; /* samples produced since t_start */
    uint16_t count = 0;
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(WAVE_BLOCK_MS));
        uint32_t want = devlink_channel_wanted_hz(s_wave);
        int64_t now = esp_timer_get_time();
        if (want != hz) { /* nobody listens (0) or a new rate: restart the sample clock */
            hz = want;
            t_start = now;
            done = 0;
            continue;
        }
        if (hz == 0u) {
            continue;
        }
        uint64_t due = (uint64_t)(now - t_start) * hz / 1000000u;
        uint8_t flags = 0;
        if ((due - done) > WAVE_MAX_SAMPLES) { /* fell behind (e.g. paused by an OTA): skip ahead and say so */
            done = due - WAVE_MAX_SAMPLES;
            flags = DEVLINK_BLOCK_SRC_GAP;
        }
        uint16_t n = (uint16_t)(due - done);
        if (n == 0u) {
            continue;
        }
        uint32_t period_ns = 1000000000u / hz;
        int64_t t0 = t_start + (int64_t)(done * 1000000u / hz);
        for (uint16_t i = 0; i < n; i++) {
            double t = (double)(t0 + ((int64_t)i * period_ns / 1000)) / 1e6;
            buf[i][0] = (int16_t)(10000.0 * sin(2.0 * M_PI * 2.0 * t));
            buf[i][1] = (int16_t)count++;
            buf[i][2] = (int16_t)((rand() % 201) - 100);
        }
        devlink_publish_block(s_wave, t0, period_ns, scale, buf, n, flags);
        done += n;
    }
}

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
    s_wave = devlink_block_register(
        &(devlink_channel_cfg_t){.name = "example.wave", .unit = "", .default_hz = 200, .max_hz = 1000},
        &(devlink_block_meta_t){.fields = "sine,count,noise", .n_fields = WAVE_FIELDS, .sample = DEVLINK_SAMPLE_I16});
    if (s_wave == NULL) {
        ESP_LOGW(TAG, "example.wave not registered (block channels off?)");
    } else if (xTaskCreate(wave_task, "mod_example_wave", 3072, NULL, 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return (xTaskCreate(example_task, "mod_example", 3072, NULL, 3, NULL) == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}
