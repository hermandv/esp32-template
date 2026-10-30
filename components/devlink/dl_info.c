/*
 * /info, /status and discovery records (SPEC §5.2, §5.5, §5.6), plus the partition-table check (§3.7).
 */
#include <inttypes.h>
#include <string.h>

#include "devlink.h"
#include "dl_ota_check.h"
#include "dl_priv.h"
#include "esp_app_desc.h"
#include "esp_bootloader_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "sdkconfig.h"

static const char *s_last_error;

void dl_note_error(const char *err)
{
    s_last_error = err;
}

const char *dl_last_error(void)
{
    return s_last_error;
}

const char *dl_fw_version(void)
{
    return esp_app_get_description()->version;
}

const char *dl_fw_project(void)
{
    return esp_app_get_description()->project_name;
}

/* ---- Partition table check (SPEC §3.7) ---- */

typedef struct {
    const char *name;
    esp_partition_type_t type;
    uint32_t offset;
} part_t;

static const part_t k_common[] = {
    {"nvs", ESP_PARTITION_TYPE_DATA, 0x9000}, {"otadata", ESP_PARTITION_TYPE_DATA, 0xF000},
    {"phy_init", ESP_PARTITION_TYPE_DATA, 0x11000}, {"dlcfg", ESP_PARTITION_TYPE_DATA, 0x12000},
    {"dlboot", ESP_PARTITION_TYPE_DATA, 0x14000}, {"factory", ESP_PARTITION_TYPE_APP, 0x20000},
};
static const part_t k_8mb[] = {
    {"ota_0", ESP_PARTITION_TYPE_APP, 0x1A0000}, {"ota_1", ESP_PARTITION_TYPE_APP, 0x4A0000},
    {"coredump", ESP_PARTITION_TYPE_DATA, 0x7A0000},
};
static const part_t k_4mb[] = {
    {"ota_0", ESP_PARTITION_TYPE_APP, 0x120000}, {"ota_1", ESP_PARTITION_TYPE_APP, 0x280000},
    {"coredump", ESP_PARTITION_TYPE_DATA, 0x3E0000},
};

static bool parts_match(const part_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        const esp_partition_t *f = esp_partition_find_first(p[i].type, ESP_PARTITION_SUBTYPE_ANY, p[i].name);
        if ((f == NULL) || (f->address != p[i].offset)) {
            return false;
        }
    }
    return true;
}

bool dl_partitions_ok(void)
{
    return parts_match(k_common, sizeof(k_common) / sizeof(k_common[0])) &&
           (parts_match(k_8mb, sizeof(k_8mb) / sizeof(k_8mb[0])) || parts_match(k_4mb, sizeof(k_4mb) / sizeof(k_4mb[0])));
}

/* ---- Slots ---- */

static const char *state_name(esp_ota_img_states_t s)
{
    switch (s) {
    case ESP_OTA_IMG_NEW:
        return "new";
    case ESP_OTA_IMG_PENDING_VERIFY:
        return "pending_verify";
    case ESP_OTA_IMG_VALID:
        return "valid";
    case ESP_OTA_IMG_INVALID:
        return "invalid";
    case ESP_OTA_IMG_ABORTED:
        return "aborted";
    default:
        return "undefined";
    }
}

static const esp_partition_t *slot(int i)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP, (esp_partition_subtype_t)(ESP_PARTITION_SUBTYPE_APP_OTA_0 + i),
                                    NULL);
}

/* How good a slot is as "the app to keep" (SPEC §6.5): 2 = proven (valid, or undefined as after a first flash),
 * 1 = not verified yet (new / pending verify: may still roll back), 0 = no image, invalid or aborted. */
static int slot_rank(int i, esp_app_desc_t *desc)
{
    const esp_partition_t *p = slot(i);
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    if ((p == NULL) || (esp_ota_get_partition_description(p, desc) != ESP_OK)) {
        return 0;
    }
    if (esp_ota_get_state_partition(p, &st) != ESP_OK) {
        st = ESP_OTA_IMG_UNDEFINED;
    }
    if ((st == ESP_OTA_IMG_INVALID) || (st == ESP_OTA_IMG_ABORTED)) {
        return 0;
    }
    return ((st == ESP_OTA_IMG_NEW) || (st == ESP_OTA_IMG_PENDING_VERIFY)) ? 1 : 2;
}

bool dl_valid_app_slot(int *out)
{
    esp_app_desc_t d;
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    int best = -1;
    int best_rank = 0;
    for (int i = 0; i < 2; i++) {
        int r = slot_rank(i, &d);
        /* the better rank wins; on a tie the one otadata selects */
        if ((r > best_rank) || ((r == best_rank) && (r > 0) && (boot != NULL) && (slot(i) == boot))) {
            best = i;
            best_rank = r;
        }
    }
    if (best < 0) {
        return false;
    }
    *out = best;
    return true;
}

/* ---- JSON ---- */

static void app_desc_json(dl_jw_t *w, const esp_app_desc_t *d)
{
    char sha[17];
    for (int i = 0; i < 8; i++) {
        static const char hex[] = "0123456789abcdef";
        sha[2 * i] = hex[d->app_elf_sha256[i] >> 4];
        sha[2 * i + 1] = hex[d->app_elf_sha256[i] & 0xF];
    }
    sha[16] = '\0';
    dl_jw_raw(w, "{");
    dl_jw_kv_str(w, "project", d->project_name);
    dl_jw_kv_str(w, "version", d->version);
    dl_jw_kv_str(w, "date", d->date);
    dl_jw_kv_str(w, "time", d->time);
    dl_jw_kv_str(w, "idf", d->idf_ver);
    dl_jw_kv_str(w, "elf_sha", sha);
    dl_jw_raw(w, "}");
}

static void records_json(dl_jw_t *w)
{
    uint8_t rec[16];
    size_t total = 0;
    size_t n = devlink_bootlog_recent(rec, sizeof(rec), &total);
    dl_jw_kv_uint(w, "records_total", total);
    dl_jw_key(w, "records");
    dl_jw_raw(w, "[");
    for (size_t i = 0; i < n; i++) {
        uint8_t r = rec[i];
        dl_jw_raw(w, (i > 0) ? ",{" : "{");
        dl_jw_kv_uint(w, "raw", r);
        dl_jw_kv_bool(w, "recovery", dl_rec_has(r, DL_REC_RECOVERY));
        dl_jw_kv_bool(w, "alive", dl_rec_has(r, DL_REC_ALIVE_5S));
        dl_jw_kv_bool(w, "stable", dl_rec_has(r, DL_REC_STABLE));
        dl_jw_kv_bool(w, "clean", dl_rec_has(r, DL_REC_CLEAN_RESTART));
        dl_jw_raw(w, "}");
    }
    dl_jw_raw(w, "]");
}

static uint32_t unconfirmed_in_a_row(void)
{
    uint8_t rec[16];
    size_t n = devlink_bootlog_recent(rec, sizeof(rec), NULL);
    uint32_t c = 0;
    /* rec[0] is this boot; count the ones before it. */
    for (size_t i = 1; (i < n) && !dl_rec_confirmed(rec[i]); i++) {
        c++;
    }
    return c;
}

void dl_info_json(dl_jw_t *w)
{
    const dl_ident_t *id = dl_ident();
    const devlink_boot_info_t *bi = devlink_boot_info();
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t flash = 0;
    (void)esp_flash_get_size(NULL, &flash);

    dl_jw_raw(w, "{");
    dl_jw_kv_str(w, "id", id->id);
    dl_jw_kv_str(w, "mac", id->mac_str);
    dl_jw_kv_str(w, "hostname", id->hostname);
    dl_jw_kv_str(w, "label", id->label);
    dl_jw_kv_str(w, "board", id->board);
    dl_jw_kv_str(w, "mode", id->mode);
    dl_jw_kv_uint(w, "api", DEVLINK_API_VERSION);

    dl_jw_key(w, "fw");
    app_desc_json(w, esp_app_get_description());

    esp_app_desc_t d;
    const esp_partition_t *factory =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    dl_jw_key(w, "recovery");
    if ((factory != NULL) && (esp_ota_get_partition_description(factory, &d) == ESP_OK)) {
        app_desc_json(w, &d);
    } else {
        dl_jw_raw(w, "null");
    }
    static const char *const selftest[] = {"none", "pending", "ok", "failed"};
    dlcfg_t st_cfg;
    (void)dlcfg_load(&st_cfg);
    dl_jw_key(w, "recovery_selftest");
    dl_jw_raw(w, "{");
    dl_jw_kv_str(w, "state", selftest[(st_cfg.rec_selftest <= 3u) ? st_cfg.rec_selftest : 0u]);
    dl_jw_kv_str(w, "version", st_cfg.rec_selftest_version);
    dl_jw_raw(w, "}");

    esp_bootloader_desc_t bd;
    dl_jw_key(w, "bootloader");
    if (esp_ota_get_bootloader_description(NULL, &bd) == ESP_OK) {
        dl_jw_raw(w, "{");
        dl_jw_kv_uint(w, "devlink_version", bd.version);
        dl_jw_kv_str(w, "idf", bd.idf_ver);
        dl_jw_kv_str(w, "date_time", bd.date_time);
        dl_jw_raw(w, "}");
    } else {
        dl_jw_raw(w, "null");
    }

    const esp_partition_t *run = esp_ota_get_running_partition();
    dl_jw_kv_str(w, "running", (run != NULL) ? run->label : "?");
    dl_jw_key(w, "slots");
    dl_jw_raw(w, "[");
    for (int i = 0; i < 2; i++) {
        const esp_partition_t *p = slot(i);
        esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
        bool has = (p != NULL) && (esp_ota_get_partition_description(p, &d) == ESP_OK);
        if ((p != NULL) && (esp_ota_get_state_partition(p, &st) != ESP_OK)) {
            st = ESP_OTA_IMG_UNDEFINED;
        }
        dl_jw_raw(w, (i > 0) ? ",{" : "{");
        dl_jw_kv_str(w, "name", (p != NULL) ? p->label : "?");
        dl_jw_kv_str(w, "state", has ? state_name(st) : "empty");
        dl_jw_kv_str(w, "version", has ? d.version : NULL);
        dl_jw_raw(w, "}");
    }
    dl_jw_raw(w, "]");
    const esp_partition_t *bad = esp_ota_get_last_invalid_partition();
    dl_jw_kv_str(w, "rejected_version",
                 ((bad != NULL) && (esp_ota_get_partition_description(bad, &d) == ESP_OK)) ? d.version : NULL);

    dl_jw_key(w, "boot");
    dl_jw_raw(w, "{");
    dl_jw_kv_str(w, "reason", dl_boot_reason_name(bi->reason));
    dl_jw_kv_bool(w, "rtc_valid", bi->rtc_valid);
    records_json(w);
    dl_jw_raw(w, "}");

    dlcfg_t cfg;
    (void)dlcfg_load(&cfg);
    dl_jw_key(w, "reset_reasons");
    dl_jw_raw(w, "[");
    for (unsigned i = 0; i < DLCFG_RESET_RING; i++) {
        unsigned k = (cfg.reset_ring_pos + DLCFG_RESET_RING - 1u - i) % DLCFG_RESET_RING; /* newest first */
        dl_jw_rawf(w, "%s%" PRIu32, (i > 0) ? "," : "", cfg.reset_ring[k]);
    }
    dl_jw_raw(w, "]");

    dl_jw_key(w, "chip");
    dl_jw_raw(w, "{");
    dl_jw_kv_str(w, "model", "ESP32-S3");
    dl_jw_kv_uint(w, "revision", chip.revision);
    dl_jw_kv_uint(w, "cores", chip.cores);
    dl_jw_kv_uint(w, "flash_size", flash);
#if CONFIG_SPIRAM
    dl_jw_kv_uint(w, "psram_size", esp_psram_get_size());
#else
    dl_jw_kv_uint(w, "psram_size", 0);
#endif
    dl_jw_raw(w, "}");
    dl_jw_kv_bool(w, "partitions_ok", dl_partitions_ok());
    dl_jw_raw(w, "}");
}

void dl_status_json(dl_jw_t *w)
{
    dl_wifi_status_t st;
    dl_ota_progress_t ota;
    char ip[16];

    dl_wifi_get_status(&st);
    dl_ota_get_progress(&ota);
    snprintf(ip, sizeof(ip), IPSTR, IP2STR(&st.ip));

    dl_jw_raw(w, "{");
    dl_jw_kv_uint(w, "uptime_ms", (unsigned long long)(esp_timer_get_time() / 1000));
    dl_jw_kv_uint(w, "heap_free", esp_get_free_heap_size());
    dl_jw_kv_uint(w, "heap_min", esp_get_minimum_free_heap_size());
    dl_jw_key(w, "wifi");
    dl_jw_raw(w, "{");
    dl_jw_kv_bool(w, "sta_connected", st.sta_connected);
    dl_jw_kv_str(w, "ssid", st.ssid);
    dl_jw_kv_str(w, "ip", st.sta_connected ? ip : NULL);
    dl_jw_kv_int(w, "rssi", st.rssi);
    dl_jw_kv_uint(w, "channel", st.channel);
    dl_jw_kv_bool(w, "ap_on", st.ap_on);
    dl_jw_kv_uint(w, "ap_clients", st.ap_clients);
    dl_jw_kv_bool(w, "pending_change", st.pending);
    dl_jw_kv_int(w, "tx_power_dbm", st.tx_power_dbm);
    dl_jw_kv_uint(w, "stack_restarts", st.stack_restarts);
    dl_jw_raw(w, "}");
    float temp;
    dl_jw_key(w, "temp");
    if (dl_stream_temperature(&temp)) {
        dl_jw_rawf(w, "%.1f", (double)temp);
    } else {
        dl_jw_raw(w, "null"); /* app with devlink_sysmon only */
    }
    dl_jw_kv_uint(w, "unconfirmed_boots", unconfirmed_in_a_row());
    dl_jw_kv_uint(w, "drops", dl_stream_drops());
    dl_jw_kv_uint(w, "ws_clients", dl_stream_clients());
    dl_jw_key(w, "ota");
    dl_jw_raw(w, "{");
    dl_jw_kv_str(w, "state", ota.state);
    dl_jw_kv_uint(w, "written", ota.written);
    dl_jw_kv_uint(w, "total", ota.total);
    dl_jw_kv_str(w, "error", ota.error);
    dl_jw_raw(w, "}");
    dl_jw_kv_bool(w, "partitions_ok", dl_partitions_ok());
    const char *err = (st.last_error != NULL) ? st.last_error : dl_last_error();
    dl_jw_kv_str(w, "last_error", err);
    dl_jw_raw(w, "}");
}

void dl_disc_json(dl_jw_t *w)
{
    const dl_ident_t *id = dl_ident();
    const devlink_boot_info_t *bi = devlink_boot_info();
    esp_app_desc_t d;
    const esp_partition_t *factory =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    bool rec = (factory != NULL) && (esp_ota_get_partition_description(factory, &d) == ESP_OK);

    dl_jw_raw(w, "{");
    dl_jw_kv_str(w, "id", id->id);
    dl_jw_kv_str(w, "mac", id->mac_str);
    dl_jw_kv_str(w, "label", id->label);
    dl_jw_kv_str(w, "board", id->board);
    dl_jw_kv_str(w, "mode", id->mode);
    dl_jw_kv_str(w, "fw", dl_fw_version());
    dl_jw_kv_str(w, "rec", rec ? d.version : "");
    dl_jw_kv_uint(w, "api", DEVLINK_API_VERSION);
    dl_jw_kv_str(w, "boot", dl_boot_reason_name(bi->reason));
    dl_jw_raw(w, "}");
}
