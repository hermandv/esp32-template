/*
 * Test console for milestone-1 hardware-in-the-loop tests (SPEC §9 tests 11-20). TEST BUILDS ONLY.
 * Line commands over the USB-Serial/JTAG console; replies start with "DLT " so the PC script can parse them.
 */
#include "devlink_testcon.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "devlink.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static devlink_testcon_cmd_fn s_extra;
static const char *s_image;

static void print_info(void)
{
    const devlink_boot_info_t *bi = devlink_boot_info();
    uint8_t recent[16];
    size_t total = 0;
    size_t n = devlink_bootlog_recent(recent, sizeof(recent), &total);

    printf("DLT info image=%s mode=%s reason=%s rtc=%d running=0x%" PRIx32 " start=0x%" PRIx32
           " index=%ld records=%u uptime=%lld recent=",
           s_image, (bi->mode == DEVLINK_MODE_APP) ? "app" : "recovery", dl_boot_reason_name(bi->reason),
           bi->rtc_valid ? 1 : 0, bi->running_offset, bi->start_offset, bi->record_index, (unsigned)total,
           esp_timer_get_time() / 1000);
    for (size_t i = 0; i < n; i++) {
        printf("%s%02X", (i == 0) ? "" : ",", recent[i]);
    }
    printf("\n");
}

/* Append a raw byte to the boot log (fault injection: torn records), or fill it. */
static void log_inject(uint8_t value, uint32_t count)
{
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x41, "dlboot");
    uint8_t sector[256];
    uint32_t end = 0;

    if (p == NULL) {
        printf("DLT err no dlboot\n");
        return;
    }
    for (uint32_t off = 0; off < 0x1000u; off += sizeof(sector)) {
        esp_partition_read(p, off, sector, sizeof(sector));
        for (uint32_t i = 0; i < sizeof(sector); i++) {
            if (sector[i] != 0xFFu) {
                end = off + i + 1u;
            }
        }
    }
    uint32_t room = 0x1000u - end;
    if (count > room) {
        count = room;
    }
    for (uint32_t i = 0; i < count; i++) {
        esp_partition_write(p, end + i, &value, 1);
    }
    printf("DLT ok injected=%" PRIu32 " end=%" PRIu32 "\n", count, end + count);
}

static void cfg_stress(void)
{
    dlcfg_t cfg;
    dlcfg_load(&cfg);
    printf("DLT ok cfg_stress start seq=%" PRIu32 "\n", dlcfg_sequence());
    for (uint32_t i = 0;; i++) {
        snprintf(cfg.label, sizeof(cfg.label), "stress %" PRIu32, dlcfg_sequence() + 1u);
        if (dlcfg_save(&cfg) != ESP_OK) {
            printf("DLT err save\n");
        }
        if ((i % 20u) == 0u) {
            printf("DLT cfg seq=%" PRIu32 "\n", dlcfg_sequence());
        }
    }
}

static void handle(char *line)
{
    char *arg = strchr(line, ' ');
    if (arg != NULL) {
        *arg++ = '\0';
    }

    if (strcmp(line, "info") == 0) {
        print_info();
    } else if (strcmp(line, "restart") == 0) {
        printf("DLT ok restart\n");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(50));
        devlink_restart();
    } else if (strcmp(line, "recovery") == 0) {
        printf("DLT ok recovery\n");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(50));
        devlink_restart_to_recovery();
    } else if (strcmp(line, "crash") == 0) {
        printf("DLT ok crash\n");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(50));
        abort();
    } else if (strcmp(line, "valid") == 0) {
        printf("DLT ok valid %s\n", esp_err_to_name(devlink_mark_valid()));
    } else if (strcmp(line, "sleep") == 0) {
        printf("DLT ok sleep\n");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(50));
        esp_deep_sleep_start();
    } else if ((strcmp(line, "log_inject") == 0) && (arg != NULL)) {
        unsigned v = 0;
        unsigned n = 1;
        if (sscanf(arg, "%x %u", &v, &n) >= 1) {
            log_inject((uint8_t)v, n);
        }
    } else if (strcmp(line, "cfg_get") == 0) {
        dlcfg_t cfg;
        esp_err_t err = dlcfg_load(&cfg);
        printf("DLT cfg err=%s seq=%" PRIu32 " label=\"%s\" ssid=\"%s\" ring_pos=%u\n", esp_err_to_name(err),
               dlcfg_sequence(), cfg.label, cfg.wifi_ssid, cfg.reset_ring_pos);
    } else if ((strcmp(line, "cfg_label") == 0) && (arg != NULL)) {
        dlcfg_t cfg;
        dlcfg_load(&cfg);
        strncpy(cfg.label, arg, sizeof(cfg.label) - 1u);
        cfg.label[sizeof(cfg.label) - 1u] = '\0';
        printf("DLT ok cfg_label %s seq=%" PRIu32 "\n", esp_err_to_name(dlcfg_save(&cfg)), dlcfg_sequence());
    } else if (strcmp(line, "cfg_stress") == 0) {
        cfg_stress();
    } else if ((s_extra != NULL) && s_extra(line, arg)) {
        /* handled by the image */
    } else if (line[0] != '\0') {
        printf("DLT err unknown '%s'\n", line);
    }
    fflush(stdout);
}

/* Lines the devlink console does not handle itself ("devlink ..." and Improv frames are handled there). */
static bool on_line(const char *in)
{
    char line[128];
    strncpy(line, in, sizeof(line) - 1u);
    line[sizeof(line) - 1u] = '\0';
    handle(line);
    return true;
}

void devlink_testcon_start(const char *image, devlink_testcon_cmd_fn extra)
{
    s_image = image;
    s_extra = extra;

    ESP_ERROR_CHECK(devlink_console_start(on_line));
    printf("DLT ready image=%s version=%s\n", image, esp_app_get_description()->version);
    print_info();
    fflush(stdout);
}
