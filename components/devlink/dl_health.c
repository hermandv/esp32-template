/*
 * Recovery self-healing (SPEC §6.4): the RTC WDT stays enabled for the whole recovery run and is fed only while the
 * HTTP server task and the Wi-Fi task show heartbeats. A stuck recovery is reset by hardware; the boot log then
 * counts it like any other failed boot.
 */
#include "dl_priv.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "dl_health";

#define RWDT_MS      30000u
#define PERIOD_MS    2000u
#define STALE_MS     15000u

static void health_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t http = dl_http_heartbeat_age_ms();
        uint32_t wifi = dl_wifi_heartbeat_age_ms();
        if ((http < STALE_MS) && (wifi < STALE_MS)) {
            dl_rwdt_feed();
        } else {
            ESP_LOGE(TAG, "stalled (http %u ms, wifi %u ms): RTC WDT will reset", (unsigned)http, (unsigned)wifi);
        }
        vTaskDelay(pdMS_TO_TICKS(PERIOD_MS));
    }
}

esp_err_t dl_health_start(void)
{
    dl_rwdt_set(RWDT_MS);
    return (xTaskCreate(health_task, "dl_health", 3072, NULL, 10, NULL) == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}
