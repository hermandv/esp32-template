/*
 * devlink config record format (SPEC §5.3). Pure C: used by the dlcfg partition driver and the host tests.
 *
 * The partition holds two 4 KB sectors with one record each: header (magic, format_version, payload_len, sequence,
 * crc32) + payload. The newest valid record by sequence wins; a write always goes to the other sector.
 * Format changes are additive only: new fields at the end of dlcfg_t, older readers ignore them.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DLCFG_MAGIC          0x47464344u /* "DCFG" */
#define DLCFG_FORMAT_VERSION 1u
#define DLCFG_SECTOR_SIZE    4096u
#define DLCFG_RESET_RING     8u

typedef struct {
    uint32_t magic;
    uint16_t format_version;
    uint16_t payload_len;
    uint32_t sequence;
    uint32_t crc;            /* dl_crc32 over the header bytes before crc, then the payload */
} dlcfg_header_t;

_Static_assert(sizeof(dlcfg_header_t) == 16, "dlcfg header layout is frozen");

/* Payload, format 1. Only append fields; never reorder or resize. */
typedef struct {
    char wifi_ssid[33];
    char wifi_pass[65];
    char prev_ssid[33];
    char prev_pass[65];
    char label[33];          /* SPEC §4: max 32 UTF-8 bytes */
    uint8_t log_level;
    uint16_t ap_window_s;    /* config-window time at boot (0 = off) */
    uint16_t button_window_s; /* config-window time after a 3 s button press */
    uint8_t reset_ring_pos;  /* next slot in reset_ring */
    uint8_t reserved[3];
    uint32_t reset_ring[DLCFG_RESET_RING]; /* esp_reset_reason_t values, newest at reset_ring_pos - 1 */
} dlcfg_t;

_Static_assert(sizeof(dlcfg_t) == 272, "dlcfg_t format 1 layout changed: fields may only be appended");
_Static_assert(sizeof(dlcfg_header_t) + sizeof(dlcfg_t) <= DLCFG_SECTOR_SIZE, "record must fit a sector");

/** Fill @p cfg with defaults (no credentials -> AP mode). */
void dlcfg_defaults(dlcfg_t *cfg);

/**
 * @brief Parse one sector. Returns true and fills @p cfg / @p seq if it holds a valid record. Fields missing in an
 *        older (shorter) record keep their defaults; extra fields from a newer format are ignored.
 *
 * @requirement SPEC-5.3
 * @parent      I3
 * @test        host:dlcfg_*, T16
 */
bool dlcfg_parse(const uint8_t *sector, size_t len, dlcfg_t *cfg, uint32_t *seq);

/**
 * @brief Pick the newest valid record of the two sectors.
 * @return 0 or 1, or -1 if neither is valid (then use defaults).
 *
 * @requirement SPEC-5.3
 * @parent      I3
 * @test        host:dlcfg_*, T16
 */
int dlcfg_select(const uint8_t *s0, const uint8_t *s1, size_t len, dlcfg_t *cfg, uint32_t *seq);

/**
 * @brief Build a record for @p cfg with sequence @p seq into @p buf (>= dlcfg_record_size()).
 *        Write it to the OTHER sector than the one dlcfg_select() returned, body first, header last.
 *
 * @requirement SPEC-5.3
 * @parent      I3
 * @test        host:dlcfg_*, T16
 */
size_t dlcfg_build(const dlcfg_t *cfg, uint32_t seq, uint8_t *buf, size_t buflen);

/** Size of a record built by this firmware. */
static inline size_t dlcfg_record_size(void)
{
    return sizeof(dlcfg_header_t) + sizeof(dlcfg_t);
}

/** Sector to write next, given the result of dlcfg_select(). */
static inline int dlcfg_write_sector(int selected)
{
    return (selected == 0) ? 1 : 0;
}

/** Push a reset reason into the ring. */
void dlcfg_push_reset_reason(dlcfg_t *cfg, uint32_t reason);

#ifdef __cplusplus
}
#endif
