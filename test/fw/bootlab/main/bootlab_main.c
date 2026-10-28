/*
 * bootlab: test app for the HIL tests (SPEC §9). Runs full devlink (self-test, API, OTA). TEST BUILD ONLY.
 *
 * Behaviour for every boot comes from NVS key "mode", set with the console command "mode <value>":
 *   normal         devlink self-test marks it valid, then idle
 *   crash:<s>      abort() at <s> seconds                                    (test 11)
 *   sleep          deep sleep forever after 1 s, before ALIVE_5S             (test 12)
 *   sleepwake:<s>[:<n>]  deep sleep with a <s> s timer after 1 s, every wake-up (or n times)  (test 13a)
 *   restart:<s>    planned restart (devlink_restart) after <s> s              (test 13b)
 *
 * Build-time fault variants for the rollback tests (test/fw/bootlab_*.defaults):
 *   CONFIG_BOOTLAB_FAULT_PANIC  panic right at start                           (test 5)
 *   CONFIG_BOOTLAB_FAULT_HANG   all of app_main blocked before devlink starts  (test 6: only the RTC WDT helps)
 *   CONFIG_DEVLINK_TEST_NO_HTTP API never starts, self-test fails after 60 s  (test 7)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "devlink.h"
#include "devlink_testcon.h"
#include "esp_attr.h"
#include "esp_sleep.h"
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "freertos/semphr.h"

static char s_mode[32] = "normal";

static void mode_load(void)
{
    nvs_handle_t h;
    size_t len = sizeof(s_mode);

    if (nvs_open("bootlab", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_str(h, "mode", s_mode, &len) != ESP_OK) {
            strcpy(s_mode, "normal");
        }
        nvs_close(h);
    }
}

/* Deep-sleep wake count, kept across deep sleep, cleared at power-on/reset. */
static RTC_DATA_ATTR uint32_t s_wakes;

static bool extra_cmd(const char *cmd, const char *arg)
{
    if (strcmp(cmd, "wakes") == 0) {
        printf("DLT ok wakes=%" PRIu32 "\n", s_wakes);
        return true;
    }
    if ((strcmp(cmd, "mode") != 0) || (arg == NULL)) {
        return false;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open("bootlab", NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_str(h, "mode", arg);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    printf("DLT ok mode %s %s\n", arg, esp_err_to_name(err));
    return true;
}

static unsigned mode_arg(const char *prefix)
{
    size_t n = strlen(prefix);
    return (strncmp(s_mode, prefix, n) == 0) ? (unsigned)strtoul(s_mode + n, NULL, 10) : 0u;
}

/* "sleepwake:<s>:<n>": stop after n sleeps (then behave like normal); no ":<n>" = forever. */
static uint32_t sleepwake_limit(void)
{
    const char *p = strchr(s_mode + 10, ':');
    return (p != NULL) ? (uint32_t)strtoul(p + 1, NULL, 10) : UINT32_MAX;
}

void app_main(void)
{
    devlink_boot_early(DEVLINK_MODE_APP);

    esp_err_t err = nvs_flash_init();
    if ((err == ESP_ERR_NVS_NO_FREE_PAGES) || (err == ESP_ERR_NVS_NEW_VERSION_FOUND)) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    mode_load();
    devlink_testcon_start("bootlab", extra_cmd);
    printf("DLT bootlab mode=%s\n", s_mode);
    fflush(stdout);

#if CONFIG_BOOTLAB_FAULT_PANIC
    printf("DLT bootlab FAULT BUILD: panic at start\n");
    fflush(stdout);
    abort();
#endif
#if CONFIG_BOOTLAB_FAULT_HANG
    printf("DLT bootlab FAULT BUILD: hanging before the self-test\n");
    fflush(stdout);
    SemaphoreHandle_t never = xSemaphoreCreateBinary();
    (void)xSemaphoreTake(never, portMAX_DELAY); /* blocked, interrupts on: the task WDT sees nothing wrong */
#endif

    if ((strncmp(s_mode, "sleepwake:", 10) == 0) && (s_wakes < sleepwake_limit())) {
        s_wakes++;
        vTaskDelay(pdMS_TO_TICKS(1000));
        printf("DLT bootlab deep sleep %u s, wake %" PRIu32 "\n", mode_arg("sleepwake:"), s_wakes);
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(20));
        esp_sleep_enable_timer_wakeup((uint64_t)mode_arg("sleepwake:") * 1000000u);
        esp_deep_sleep_start();
    }
    if (strcmp(s_mode, "sleep") == 0) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        printf("DLT bootlab deep sleep forever\n");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(20));
        esp_deep_sleep_start();
    }

    devlink_start(DEVLINK_MODE_APP); /* Wi-Fi, API; the self-test marks this app valid */
    vTaskDelay(pdMS_TO_TICKS(2000));

    unsigned crash_s = mode_arg("crash:");
    unsigned restart_s = mode_arg("restart:");
    if ((crash_s > 0u) || (restart_s > 0u)) {
        unsigned at = (crash_s > 0u) ? crash_s : restart_s;
        vTaskDelay(pdMS_TO_TICKS(at * 1000u - 2000u));
        printf("DLT bootlab %s at %u s\n", (crash_s > 0u) ? "crash" : "restart", at);
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(20));
        if (crash_s > 0u) {
            abort();
        }
        devlink_restart();
    }
}
