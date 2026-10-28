/*
 * Built-in app channels (SPEC §5.8). Each channel is sampled at the highest rate a client asked for, and not at
 * all while nobody listens. The temperature is also kept for /status.
 */
#include "devlink_sysmon.h"

#include "devlink_stream.h"
#include "driver/temperature_sensor.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "sysmon";

#define TICK_MS          20u
#define TEMP_STATUS_MS   5000u

typedef struct {
    devlink_channel_t *ch;
    uint64_t next_us;
} sampler_t;

static temperature_sensor_handle_t s_tsens;
static sampler_t s_temp, s_heap, s_rssi;

static float read_temp(void)
{
    float c = 0.0f;
    if ((s_tsens == NULL) || (temperature_sensor_get_celsius(s_tsens, &c) != ESP_OK)) {
        return -1000.0f;
    }
    return c;
}

/* True when this sampler should take a sample now (at the rate the clients want). */
static bool due(sampler_t *s, uint64_t now)
{
    uint32_t hz = devlink_channel_wanted_hz(s->ch);
    if ((hz == 0u) || (now < s->next_us)) {
        return false;
    }
    uint64_t period = 1000000u / hz;
    /* Steady cadence (no drift from the task tick); after a pause start again from now. */
    s->next_us = (s->next_us + period > now) ? s->next_us + period : now + period;
    return true;
}

static void sysmon_task(void *arg)
{
    (void)arg;
    uint64_t next_status = 0;
    for (;;) {
        uint64_t now = (uint64_t)esp_timer_get_time();
        if (due(&s_temp, now) || (now >= next_status)) {
            float c = read_temp();
            if (c > -999.0f) {
                devlink_set_temperature(c);
                devlink_publish_num(s_temp.ch, c);
            }
            next_status = now + TEMP_STATUS_MS * 1000u;
        }
        if (due(&s_heap, now)) {
            devlink_publish_num(s_heap.ch, (double)heap_caps_get_free_size(MALLOC_CAP_DEFAULT));
        }
        if (due(&s_rssi, now)) {
            wifi_ap_record_t ap;
            if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
                devlink_publish_num(s_rssi.ch, ap.rssi);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
}

esp_err_t devlink_sysmon_start(void)
{
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    if ((temperature_sensor_install(&cfg, &s_tsens) != ESP_OK) || (temperature_sensor_enable(s_tsens) != ESP_OK)) {
        ESP_LOGW(TAG, "temperature sensor not available");
        s_tsens = NULL;
    }
    s_temp.ch = devlink_channel_register(
        &(devlink_channel_cfg_t){.name = "sys.temp", .unit = "degC", .type = DL_NUM, .default_hz = 1, .max_hz = 10});
    s_heap.ch = devlink_channel_register(
        &(devlink_channel_cfg_t){.name = "sys.heap", .unit = "B", .type = DL_NUM, .default_hz = 1, .max_hz = 10});
    s_rssi.ch = devlink_channel_register(
        &(devlink_channel_cfg_t){.name = "sys.rssi", .unit = "dBm", .type = DL_NUM, .default_hz = 1, .max_hz = 5});
    return (xTaskCreate(sysmon_task, "dl_sysmon", 3072, NULL, 3, NULL) == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}
