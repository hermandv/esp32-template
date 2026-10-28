/*
 * devlink network services start-up (SPEC §5, §6): identity, Wi-Fi + config AP, HTTP API, captive DNS, discovery,
 * and in recovery the health task. Called after devlink_boot_early().
 */
#include "devlink.h"
#include "dl_priv.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "devlink";

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
    if (recovery) {
        (void)dl_health_start(); /* even if something above failed: a stuck recovery must get reset */
    } else {
        (void)dl_app_start(); /* self-test ends the verify window (SPEC 3.5), button (5.9) */
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "network start failed: %s", esp_err_to_name(err));
        dl_note_error("network_start_failed");
    }
    return err;
}
