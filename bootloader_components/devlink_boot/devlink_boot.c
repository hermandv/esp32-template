/*
 * devlink bootloader override glue (SPEC §3.4): flash boot log, retained RTC, decision -> partition index.
 * Bootloader build only. The rules themselves are in dl_bootlog.c (host-tested).
 */
#include "devlink_boot.h"

#include <string.h>

#include "bootloader_common.h"
#include "bootloader_flash_priv.h"
#include "bootloader_utility.h"
#include "dl_bootlog.h"
#include "dl_rtc.h"
#include "esp_flash_partitions.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "sdkconfig.h"

#define DL_PART_SUBTYPE_DLBOOT 0x41u
#define DL_BOOTLOG_SECTOR      0x1000u

#ifndef CONFIG_DEVLINK_BOOT_RECOVERY_GPIO
#define CONFIG_DEVLINK_BOOT_RECOVERY_GPIO -1
#endif

ESP_LOG_ATTR_TAG(TAG, "devlink");

_Static_assert(CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC_SIZE == sizeof(dl_rtc_t),
               "CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC_SIZE must equal sizeof(dl_rtc_t)");
/* The images read the devlink bootloader version from the IDF bootloader descriptor. */
_Static_assert(CONFIG_BOOTLOADER_PROJECT_VER == DEVLINK_BOOT_VERSION,
               "CONFIG_BOOTLOADER_PROJECT_VER (tools/sdkconfig.defaults.devlink) must equal DEVLINK_BOOT_VERSION");

/* Find the dlboot partition in the partition table. Returns false if missing. */
static bool find_dlboot(esp_partition_pos_t *pos)
{
    const esp_partition_info_t *tbl = bootloader_mmap(ESP_PARTITION_TABLE_OFFSET, ESP_PARTITION_TABLE_MAX_LEN);
    bool found = false;

    if (tbl == NULL) {
        return false;
    }
    for (size_t i = 0; i < ESP_PARTITION_TABLE_MAX_ENTRIES; i++) {
        if (tbl[i].magic != ESP_PARTITION_MAGIC) {
            break;
        }
        if ((tbl[i].type == PART_TYPE_DATA) && (tbl[i].subtype == DL_PART_SUBTYPE_DLBOOT) &&
            (tbl[i].pos.size >= DL_BOOTLOG_SECTOR)) {
            *pos = tbl[i].pos;
            found = true;
            break;
        }
    }
    bootloader_munmap(tbl);
    return found;
}

/* Program @p len bytes at @p addr (any alignment): writes whole words, other bytes 0xFF (unchanged on NOR flash). */
static esp_err_t program_bytes(uint32_t addr, const uint8_t *data, size_t len)
{
    uint32_t words[4];
    uint32_t start = addr & ~3u;
    uint32_t span = ((addr + (uint32_t)len + 3u) & ~3u) - start;

    if (span > sizeof(words)) {
        return ESP_ERR_INVALID_SIZE;
    }
    memset(words, 0xFF, sizeof(words));
    memcpy((uint8_t *)words + (addr - start), data, len);
    return bootloader_flash_write(start, words, span, false);
}

static void append_record(const esp_partition_pos_t *log, const dl_bootlog_scan_t *scan, uint8_t rec)
{
    dl_bootlog_append_t plan;
    dl_bootlog_plan_append(scan, DL_BOOTLOG_SECTOR, rec, &plan);

    if (plan.erase) {
        ESP_LOGI(TAG, "boot log full, rolling over");
        if (bootloader_flash_erase_sector(log->offset / DL_BOOTLOG_SECTOR) != ESP_OK) {
            ESP_LOGE(TAG, "boot log erase failed");
            return;
        }
    }
    /* Carry-over (<= 9 bytes) in at most two 16-byte chunks. */
    for (size_t done = 0; done < plan.len;) {
        size_t n = plan.len - done;
        if (n > 12u) {
            n = 12u;
        }
        if (program_bytes(log->offset + (uint32_t)(plan.offset + done), &plan.data[done], n) != ESP_OK) {
            ESP_LOGE(TAG, "boot log write failed");
            return;
        }
        done += n;
    }
}

static bool recovery_gpio_held(void)
{
#if CONFIG_DEVLINK_BOOT_RECOVERY_GPIO >= 0
    return bootloader_common_check_long_hold_gpio_level(CONFIG_DEVLINK_BOOT_RECOVERY_GPIO, 0, false) != GPIO_NOT_HOLD;
#else
    return false;
#endif
}

/* True if the active otadata entry is still PENDING_VERIFY: the IDF selection is about to roll it back. */
static bool rollback_pending(const bootloader_state_t *bs)
{
    esp_ota_select_entry_t od[2];

    if ((bs->ota_info.offset == 0u) || (bootloader_common_read_otadata(&bs->ota_info, od) != ESP_OK)) {
        return false;
    }
    int active = bootloader_common_get_active_otadata(od);
    return (active >= 0) && (od[active].ota_state == ESP_OTA_IMG_PENDING_VERIFY);
}

static uint32_t index_offset(const bootloader_state_t *bs, int index)
{
    if (index == FACTORY_INDEX) {
        return bs->factory.offset;
    }
    if ((index >= 0) && ((uint32_t)index < bs->app_count)) {
        return bs->ota[index].offset;
    }
    return 0u;
}

int devlink_boot_select(bootloader_state_t *bs)
{
    rtc_retain_mem_t *mem = bootloader_common_get_rtc_retain_mem();
    dl_rtc_t rtc;
    dl_bootlog_scan_t scan = {0};
    esp_partition_pos_t log = {0};

    /* Inputs. Read our RTC block before anything below may reset the retain area. */
    memcpy(&rtc, mem->custom, sizeof(rtc));
    bool req = dl_rtc_valid(&rtc) && (rtc.req == DL_RTC_REQ_RECOVERY);
    soc_reset_reason_t rst = esp_rom_get_reset_reason(0);
    bool deep_sleep = (rst == RESET_REASON_CORE_DEEP_SLEEP);
    /* Reset by the PC over USB-Serial/JTAG (esptool / idf.py DTR-RTS, or JTAG): a planned restart. */
    bool host_reset = (rst == RESET_REASON_CORE_USB_UART) || (rst == RESET_REASON_CORE_USB_JTAG);
    bool have_log = find_dlboot(&log);

    if (have_log) {
        const uint8_t *sector = bootloader_mmap(log.offset, DL_BOOTLOG_SECTOR);
        if (sector != NULL) {
            dl_bootlog_scan(sector, DL_BOOTLOG_SECTOR, &scan);
            bootloader_munmap(sector);
        } else {
            have_log = false;
        }
    }
    if (!have_log) {
        ESP_LOGE(TAG, "dlboot partition missing or unreadable: standard boot");
    }

    dl_boot_inputs_t in = {
        .deep_sleep_wake = deep_sleep,
        .host_reset = host_reset,
        .req_recovery = req,
        .gpio_recovery = recovery_gpio_held(),
        .log = &scan,
    };
    dl_boot_decision_t d = dl_boot_decide(&in);

    if (have_log && d.clean_prev && (scan.end > 0u)) {
        uint8_t clean = (uint8_t)~DL_REC_CLEAN_RESTART;
        if (program_bytes(log.offset + (uint32_t)(scan.end - 1u), &clean, 1) != ESP_OK) {
            ESP_LOGE(TAG, "boot log clean mark failed");
        }
    }
    if (have_log && d.append) {
        append_record(&log, &scan, d.record);
    }

    /* Partition index. */
    int index;
    dl_boot_reason_t reason = d.reason;
    if ((d.target == DL_BOOT_TARGET_FACTORY) && (bs->factory.offset != 0u)) {
        index = FACTORY_INDEX; /* otadata untouched: slot states are kept */
    } else {
        if (rollback_pending(bs)) {
            reason = DL_BOOT_REASON_ROLLBACK;
        }
        index = bootloader_utility_get_selected_boot_partition(bs);
        if ((index == FACTORY_INDEX) && (d.target == DL_BOOT_TARGET_APP) && (bs->app_count > 0u)) {
            index = (int)bs->app_count - 1; /* tries the slots downwards, factory last */
        } else if ((index == FACTORY_INDEX) && (reason == DL_BOOT_REASON_NORMAL)) {
            reason = DL_BOOT_REASON_NO_VALID_APP;
        }
    }

    /* Hand over to the image. Make IDF's retain area valid first: otherwise the IDF loader resets the whole area
     * (including our custom block) just before starting the image on the first boot after power-on. */
    bootloader_common_update_rtc_retain_mem(NULL, true);
    dl_rtc_init(&rtc);
    rtc.boot_reason = (uint8_t)reason;
    rtc.target = (uint8_t)d.target;
    rtc.start_offset = index_offset(bs, index);
    dl_rtc_seal(&rtc);
    memcpy(mem->custom, &rtc, sizeof(rtc));

    ESP_LOGI(TAG, "boot v%d: reason=%s records=%u%s%s -> index %d (0x%x)", DEVLINK_BOOT_VERSION,
             dl_boot_reason_name(reason), (unsigned)scan.count, req ? " req" : "", d.clean_prev ? " host" : "",
             index, (unsigned)rtc.start_offset);
    return index;
}
