/*
 * devlink retained-RTC block (SPEC §3.4): one-shot REQ_RECOVERY flag (image -> bootloader) and boot reason
 * (bootloader -> image). Lives in rtc_retain_mem_t.custom (CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC_SIZE = 16).
 * IDF's own CRC does not cover the custom area, so the block carries its own magic + CRC.
 *
 * Pure C. Layout FROZEN with the bootloader.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DL_RTC_MAGIC       0x544C4C44u /* "DLLT" */
#define DL_RTC_VERSION     1u
#define DL_RTC_REQ_NONE    0x00u
#define DL_RTC_REQ_RECOVERY 0xA5u

typedef struct {
    uint32_t magic;
    uint8_t version;
    uint8_t req;            /* DL_RTC_REQ_*: set by app/recovery, cleared by the bootloader */
    uint8_t boot_reason;    /* dl_boot_reason_t, set by the bootloader */
    uint8_t target;         /* dl_boot_target_t chosen by the bootloader */
    uint32_t start_offset;  /* flash offset of the partition the bootloader started from (0 = unknown) */
    uint32_t crc;           /* dl_crc32 over the bytes before it */
} dl_rtc_t;

_Static_assert(sizeof(dl_rtc_t) == 16, "dl_rtc_t must fill the 16-byte custom RTC area exactly");

/** CRC-32 (IEEE, reflected, init/xorout 0xFFFFFFFF). Small bitwise version, also used by dlcfg. */
uint32_t dl_crc32(const void *data, uint32_t len);

/** Streaming form: state = DL_CRC32_INIT, update over each piece, result = ~state. */
#define DL_CRC32_INIT 0xFFFFFFFFu
uint32_t dl_crc32_update(uint32_t state, const void *data, uint32_t len);

/** True if the block has the right magic, version and CRC (garbage after power-on is invalid). */
bool dl_rtc_valid(const dl_rtc_t *rtc);

/** Recompute the CRC after changing fields. */
void dl_rtc_seal(dl_rtc_t *rtc);

/** Reset to an empty, sealed block. */
void dl_rtc_init(dl_rtc_t *rtc);

#ifdef __cplusplus
}
#endif
