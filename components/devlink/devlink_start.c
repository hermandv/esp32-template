/*
 * devlink network services start-up (SPEC §5, §6): identity, Wi-Fi + config AP, HTTP API, captive DNS, discovery,
 * and in recovery the health task. Called after devlink_boot_early().
 */
#include "devlink.h"
#include "dl_priv.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "devlink";

#define SELFTEST_LIMIT_MS 30000u

static void set_selftest(uint8_t result)
{
    dlcfg_t cfg;
    if (dlcfg_load(&cfg) == ESP_OK) {
        cfg.rec_selftest = result;
        (void)dlcfg_save(&cfg);
    }
}

/* Recovery self-test boot after a recovery update (SPEC §7.3): Wi-Fi (AP) and HTTP up within 30 s -> ok, then a
 * planned restart back into the app (the one-shot request is used up). A crash or hang never gets here: the app then
 * finds the self-test still pending and records it as failed. */
static void recovery_selftest_task(void *arg)
{
    (void)arg;
    uint8_t result = DLCFG_SELFTEST_FAILED;
    for (uint32_t t = 0; t < SELFTEST_LIMIT_MS; t += 500u) {
        dl_wifi_status_t st;
        dl_wifi_get_status(&st);
        if (st.ap_on && (dl_http_heartbeat_age_ms() < 5000u)) {
            result = DLCFG_SELFTEST_OK;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    set_selftest(result);
    ESP_LOGW(TAG, "recovery self-test %s: back to the app", (result == DLCFG_SELFTEST_OK) ? "OK" : "FAILED");
    vTaskDelay(pdMS_TO_TICKS(300));
    devlink_restart();
}

esp_err_t devlink_start(devlink_mode_t mode)
{
    bool recovery = (mode == DEVLINK_MODE_RECOVERY);
    esp_err_t err;

    /* identity was set up in devlink_boot_early() */
    if (!dl_partitions_ok()) {
        dl_note_error("partition_table_mismatch");
        ESP_LOGE(TAG, "partition table does not match the frozen devlink layout (SPEC 3.7)");
    }
    err = dl_wifi_start(recovery);
#if !CONFIG_DEVLINK_TEST_NO_HTTP
    if (err == ESP_OK) {
        err = dl_http_start();
    }
#else
    ESP_LOGW(TAG, "TEST BUILD: HTTP API not started (self-test must fail)");
#endif
    if (err == ESP_OK) {
        (void)dl_dns_start();
        (void)dl_disc_start();
    }
    dlcfg_t cfg;
    bool selftest_pending = (dlcfg_load(&cfg) == ESP_OK) && (cfg.rec_selftest == DLCFG_SELFTEST_PENDING);
    if (recovery) {
        (void)dl_health_start(); /* even if something above failed: a stuck recovery must get reset */
        if (selftest_pending) {
            (void)xTaskCreate(recovery_selftest_task, "dl_rec_test", 3072, NULL, 4, NULL);
        }
    } else {
        /* (A self-test still pending in the app is turned into "failed" in devlink_boot_early().) */
        (void)dl_app_start(); /* self-test ends the verify window (SPEC 3.5), button (5.9) */
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "network start failed: %s", esp_err_to_name(err));
        dl_note_error("network_start_failed");
    }
    return err;
}
