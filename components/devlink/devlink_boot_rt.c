/*
 * devlink runtime boot steps (SPEC §3.4, §3.5): RTC WDT take-over, boot info, boot-log marks, planned restarts.
 */
#include <string.h>

#include "bootloader_common.h"
#include "devlink.h"
#include "dl_ota_check.h"
#include "dl_priv.h"
#include "dl_rtc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "hal/wdt_hal.h"
#include "soc/rtc.h"

#define DL_PART_SUBTYPE_DLBOOT ((esp_partition_subtype_t)0x41)
#define DL_BOOTLOG_SECTOR      0x1000u
#define CHUNK                  256u

static const char *TAG = "devlink";

extern const dl_marker_t devlink_marker; /* dl_ident.c */

static devlink_boot_info_t s_info;
static const esp_partition_t *s_log;
static SemaphoreHandle_t s_lock;
static esp_timer_handle_t s_alive_timer;
static esp_timer_handle_t s_stable_timer;
static bool s_valid;
static bool s_stable_due;

_Static_assert(CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC_SIZE == sizeof(dl_rtc_t),
               "CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC_SIZE must equal sizeof(dl_rtc_t)");

/* ---- RTC WDT ---- */

void dl_rwdt_set(uint32_t ms)
{
    wdt_hal_context_t ctx = RWDT_HAL_CONTEXT_DEFAULT();
    uint32_t ticks = (uint32_t)((uint64_t)ms * rtc_clk_slow_freq_get_hz() / 1000u);

    wdt_hal_write_protect_disable(&ctx);
    wdt_hal_config_stage(&ctx, WDT_STAGE0, ticks, WDT_STAGE_ACTION_RESET_RTC);
    wdt_hal_enable(&ctx);
    wdt_hal_feed(&ctx);
    wdt_hal_write_protect_enable(&ctx);
}

void dl_rwdt_feed(void)
{
    wdt_hal_context_t ctx = RWDT_HAL_CONTEXT_DEFAULT();

    wdt_hal_write_protect_disable(&ctx);
    wdt_hal_feed(&ctx);
    wdt_hal_write_protect_enable(&ctx);
}

void dl_rwdt_disable(void)
{
    wdt_hal_context_t ctx = RWDT_HAL_CONTEXT_DEFAULT();

    wdt_hal_write_protect_disable(&ctx);
    wdt_hal_disable(&ctx);
    wdt_hal_write_protect_enable(&ctx);
}

/* ---- Boot log access ---- */

/* Index of the newest record (= this boot's), scanning backwards in chunks. -1 if the log is empty. */
static long find_newest(void)
{
    uint8_t buf[CHUNK];

    for (uint32_t end = DL_BOOTLOG_SECTOR; end > 0u; end -= CHUNK) {
        if (esp_partition_read(s_log, end - CHUNK, buf, CHUNK) != ESP_OK) {
            return -1;
        }
        for (uint32_t i = CHUNK; i > 0u; i--) {
            if (dl_rec_present(buf[i - 1u])) {
                return (long)(end - CHUNK + i - 1u);
            }
        }
    }
    return -1;
}

size_t devlink_bootlog_recent(uint8_t *out, size_t max, size_t *total)
{
    static uint8_t sector[DL_BOOTLOG_SECTOR]; /* shared: guarded by s_lock */
    size_t n = 0u;

    if ((s_log == NULL) || (s_lock == NULL)) {
        return 0u;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (esp_partition_read(s_log, 0, sector, sizeof(sector)) == ESP_OK) {
        dl_bootlog_scan_t scan;
        dl_bootlog_scan(sector, sizeof(sector), &scan);
        for (size_t i = scan.end; (i > 0u) && (n < max); i--) {
            if (dl_rec_present(sector[i - 1u])) {
                out[n++] = sector[i - 1u];
            }
        }
        if (total != NULL) {
            *total = scan.count;
        }
    }
    xSemaphoreGive(s_lock);
    return n;
}

esp_err_t devlink_bootlog_mark(uint8_t flags)
{
    esp_err_t err = ESP_ERR_NOT_FOUND;

    if ((s_log == NULL) || (s_info.record_index < 0)) {
        return err;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint8_t rec;
    err = esp_partition_read(s_log, (size_t)s_info.record_index, &rec, 1);
    if ((err == ESP_OK) && ((rec & flags) != 0u)) {
        rec = (uint8_t)(rec & ~flags);
        err = esp_partition_write(s_log, (size_t)s_info.record_index, &rec, 1);
    }
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "boot log mark 0x%02x failed: %s", flags, esp_err_to_name(err));
    }
    return err;
}

static void alive_cb(void *arg)
{
    (void)arg;
    (void)devlink_bootlog_mark(DL_REC_ALIVE_5S);
}

static void stable_cb(void *arg)
{
    (void)arg;
    s_stable_due = true;
    if (s_valid) {
        (void)devlink_bootlog_mark(DL_REC_STABLE);
        ESP_LOGI(TAG, "stable");
    }
}

/* ---- RTC hand-over ---- */

static dl_rtc_t *rtc_block(void)
{
    return (dl_rtc_t *)(void *)bootloader_common_get_rtc_retain_mem()->custom;
}

esp_err_t devlink_boot_early(devlink_mode_t mode)
{
    /* 1. RTC WDT take-over: the bootloader's 9 s become the verify window (SPEC §3.5 rule 2). */
    dl_rwdt_set(DEVLINK_VERIFY_WDT_MS);

    /* Every devlink image calls this, so referencing the marker here keeps it in every image (SPEC §7.1). */
    __asm__ volatile("" : : "r"(&devlink_marker));

    s_lock = xSemaphoreCreateMutex();
    memset(&s_info, 0, sizeof(s_info));
    s_info.mode = mode;
    s_info.record_index = -1;

    /* 2. Boot info from the bootloader. */
    dl_rtc_t rtc;
    memcpy(&rtc, rtc_block(), sizeof(rtc));
    s_info.rtc_valid = dl_rtc_valid(&rtc);
    s_info.reason = s_info.rtc_valid ? (dl_boot_reason_t)rtc.boot_reason : DL_BOOT_REASON_UNKNOWN;
    s_info.start_offset = s_info.rtc_valid ? rtc.start_offset : 0u;
    const esp_partition_t *running = esp_ota_get_running_partition();
    s_info.running_offset = (running != NULL) ? running->address : 0u;
    if (s_info.rtc_valid && (s_info.start_offset != 0u) && (s_info.start_offset != s_info.running_offset) &&
        (s_info.reason == DL_BOOT_REASON_NORMAL)) {
        /* The loader rejected the chosen image and fell back (SPEC §3.4 rule 5). */
        s_info.reason = DL_BOOT_REASON_NO_VALID_APP;
    }
    /* Consume a request the bootloader did not see (cannot normally happen). */
    if (s_info.rtc_valid && (rtc.req != DL_RTC_REQ_NONE)) {
        rtc.req = DL_RTC_REQ_NONE;
        dl_rtc_seal(&rtc);
        memcpy(rtc_block(), &rtc, sizeof(rtc));
    }

    /* 3. Boot log. */
    s_log = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, DL_PART_SUBTYPE_DLBOOT, "dlboot");
    if (s_log == NULL) {
        ESP_LOGE(TAG, "dlboot partition missing");
    } else if (esp_reset_reason() != ESP_RST_DEEPSLEEP) {
        s_info.record_index = find_newest();
        if (mode == DEVLINK_MODE_RECOVERY) {
            (void)devlink_bootlog_mark(DL_REC_RECOVERY); /* also covers loader fallbacks into factory */
        }
        s_info.n_recent = devlink_bootlog_recent(s_info.recent, sizeof(s_info.recent), &s_info.record_count);
    } else {
        s_info.record_index = find_newest(); /* deep-sleep wake: same boot session, record not new */
    }

    const esp_timer_create_args_t alive = {.callback = alive_cb, .name = "dl_alive"};
    const esp_timer_create_args_t stable = {.callback = stable_cb, .name = "dl_stable"};
    ESP_ERROR_CHECK(esp_timer_create(&alive, &s_alive_timer));
    ESP_ERROR_CHECK(esp_timer_create(&stable, &s_stable_timer));
    ESP_ERROR_CHECK(esp_timer_start_once(s_alive_timer, (uint64_t)DL_ALIVE_MS * 1000u));
    ESP_ERROR_CHECK(esp_timer_start_once(s_stable_timer, (uint64_t)DL_STABLE_MS * 1000u));

    /* 4. Reset reason ring in dlcfg (SPEC §5.2). Not on deep-sleep wakes (flash wear). */
    dlcfg_t cfg;
    esp_err_t cfg_err = dlcfg_load(&cfg);
    if ((esp_reset_reason() != ESP_RST_DEEPSLEEP) && (cfg_err == ESP_OK)) {
        dlcfg_push_reset_reason(&cfg, (uint32_t)esp_reset_reason());
        /* The app runs while a recovery self-test is still pending: the new recovery never reported back
         * (crashed or hung), SPEC §7.3. Decided here, before anything can read /info. */
        if ((mode == DEVLINK_MODE_APP) && (cfg.rec_selftest == DLCFG_SELFTEST_PENDING)) {
            cfg.rec_selftest = DLCFG_SELFTEST_FAILED;
            ESP_LOGE(TAG, "recovery self-test FAILED (recovery %s did not report back)", cfg.rec_selftest_version);
        }
        (void)dlcfg_save(&cfg);
    }

    /* 5. Identity (SPEC §4), for every image, also those that never start the network. */
    dl_ident_init(mode == DEVLINK_MODE_RECOVERY);
    dl_ident_set_label(cfg.label);

    ESP_LOGI(TAG, "%s: boot reason=%s record=%ld/%u running=0x%" PRIx32,
             (mode == DEVLINK_MODE_APP) ? "app" : "recovery", dl_boot_reason_name(s_info.reason),
             s_info.record_index, (unsigned)s_info.record_count, s_info.running_offset);
    return ESP_OK;
}

esp_err_t devlink_mark_valid(void)
{
    esp_err_t err = ESP_OK;

    if (s_info.mode == DEVLINK_MODE_APP) {
        esp_ota_img_states_t state;
        const esp_partition_t *running = esp_ota_get_running_partition();
        if ((esp_ota_get_state_partition(running, &state) == ESP_OK) && (state == ESP_OTA_IMG_PENDING_VERIFY)) {
            err = esp_ota_mark_app_valid_cancel_rollback();
            ESP_LOGI(TAG, "app marked valid: %s", esp_err_to_name(err));
        }
    }
    if (err == ESP_OK) {
        if (s_info.mode == DEVLINK_MODE_APP) {
            dl_rwdt_disable(); /* recovery: the health task keeps feeding it (SPEC 6.4) */
        }
        s_valid = true;
        if (s_stable_due) {
            (void)devlink_bootlog_mark(DL_REC_STABLE);
        }
    }
    return err;
}

void devlink_restart(void)
{
    (void)devlink_bootlog_mark(DL_REC_CLEAN_RESTART);
    esp_restart();
}

void devlink_restart_to_recovery(void)
{
    dl_rtc_t rtc;
    dl_rtc_init(&rtc);
    rtc.req = DL_RTC_REQ_RECOVERY;
    dl_rtc_seal(&rtc);
    memcpy(rtc_block(), &rtc, sizeof(rtc));
    devlink_restart();
}

const devlink_boot_info_t *devlink_boot_info(void)
{
    return &s_info;
}
