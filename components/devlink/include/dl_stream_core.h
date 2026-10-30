/*
 * Streaming building blocks (SPEC §5.7). Pure C: frame ring per client, rate limiting, frame formatting, and parsing
 * of ESP-IDF log lines for the "log" channel.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dl_json.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Frame ring: fixed slots, never grows; a full ring drops the new frame and counts it ---- */

#define DL_RING_SLOTS     32u
#define DL_FRAME_MAX      256u

typedef struct {
    char slot[DL_RING_SLOTS][DL_FRAME_MAX];
    uint16_t len[DL_RING_SLOTS];
    uint32_t head;      /* next to read */
    uint32_t count;
    uint32_t drops;     /* frames dropped because the ring was full or the frame too long */
} dl_ring_t;

void dl_ring_init(dl_ring_t *r);
/** Copy a frame in; false (and drops++) if full or longer than DL_FRAME_MAX - 1. */
bool dl_ring_push(dl_ring_t *r, const char *frame, size_t len);
/** Append frames to @p out (newline-separated) while they fit in @p size; returns bytes written. */
size_t dl_ring_pop_batch(dl_ring_t *r, char *out, size_t size);

/* ---- Binary blocks (block channels, SPEC §5.7): one binary WebSocket message per block ----
 * Little-endian. Fixed header, then n_fields float32 scales (value = raw * scale), then n_samples x n_fields
 * samples, interleaved:
 *   0 u16 magic 0xB10C | 2 u8 version 1 | 3 u8 sample type | 4 u16 channel id | 6 u8 n_fields | 7 u8 flags
 *   8 u32 seq | 12 u64 t0_us | 20 u32 period_ns | 24 u16 n_samples | 26 u16 header length (28 + 4 * n_fields)
 */

#define DL_BLOCK_MAGIC        0xB10Cu
#define DL_BLOCK_VERSION      1u
#define DL_BLOCK_FIXED_HDR    28u
#define DL_BLOCK_MAX          1400u  /* whole message, header included */
#define DL_BLOCK_FLAG_GAP     0x01u  /* this client lost blocks of this channel before this one (ring full) */
#define DL_BLOCK_FLAG_SRC_GAP 0x02u  /* the producer lost samples (e.g. a sensor FIFO overflow) */

typedef enum { DL_SAMPLE_I16 = 0, DL_SAMPLE_I32 = 1, DL_SAMPLE_F32 = 2 } dl_sample_t;

typedef struct {
    uint8_t sample;     /* dl_sample_t */
    uint16_t ch_id;
    uint8_t n_fields;
    uint8_t flags;
    uint32_t seq;
    uint64_t t0_us;
    uint32_t period_ns;
    uint16_t n_samples;
} dl_block_hdr_t;

size_t dl_sample_size(uint8_t sample);

/**
 * @brief Write header + scales into @p out (>= DL_BLOCK_FIXED_HDR + 4 * n_fields bytes); returns the header length.
 *        @p scale NULL = 1.0 for every field.
 *
 * @requirement SPEC-5.7-BLOCK
 * @parent      -
 * @test        host:stream_block_*
 */
size_t dl_block_header(uint8_t *out, const dl_block_hdr_t *h, const float *scale);

/** Parse a block message (tests and tools); false unless header, version and total length are consistent.
 *  @p scales points at the little-endian float32 scales inside @p msg (unaligned: copy before use). */
bool dl_block_parse(const uint8_t *msg, size_t len, dl_block_hdr_t *h, const uint8_t **scales, const uint8_t **data);

/* Block ring: like the frame ring, but the slots live in memory the caller provides (allocated when a client first
 * subscribes to a block channel, so boards that never use blocks pay nothing). A full ring drops the new block. */
typedef struct {
    uint8_t *mem;       /* slots * DL_BLOCK_MAX bytes; NULL = not allocated */
    uint16_t *len;      /* slots entries */
    uint32_t slots;
    uint32_t head;
    uint32_t count;
    uint32_t drops;
} dl_bring_t;

/** Size of the memory dl_bring_init needs for @p slots. */
size_t dl_bring_mem_size(uint32_t slots);
/** @p mem: dl_bring_mem_size(slots) bytes, or NULL for an empty ring that drops everything. */
void dl_bring_init(dl_bring_t *r, void *mem, uint32_t slots);
/** Copy header + data into one slot; false (and drops++) if full, not allocated, or longer than DL_BLOCK_MAX. */
bool dl_bring_push(dl_bring_t *r, const uint8_t *hdr, size_t hlen, const void *data, size_t dlen);
/** Oldest block into @p out; returns its length, 0 if empty (or if @p size is too small). */
size_t dl_bring_pop(dl_bring_t *r, uint8_t *out, size_t size);

/* ---- Rate limiting ---- */

/**
 * @brief True if a sample at @p now_us may be sent at @p hz (0 = every sample/event). Advances *next_us.
 *
 * @requirement SPEC-5.7
 * @parent      -
 * @test        host:stream_*, T3
 */
bool dl_rate_due(uint64_t *next_us, uint64_t now_us, uint32_t hz);

/* ---- Frames (one JSON object, no newline) ---- */

void dl_frame_num(dl_jw_t *w, const char *ch, int64_t ts, double v);
void dl_frame_arr(dl_jw_t *w, const char *ch, int64_t ts, const double *v, size_t n);
/** @p json must be a valid JSON value (object, array, string, ...). */
void dl_frame_raw(dl_jw_t *w, const char *ch, int64_t ts, const char *json);
void dl_frame_log(dl_jw_t *w, int64_t ts, char lvl, const char *tag, size_t tag_len, const char *msg,
                  size_t msg_len);

/* ---- ESP-IDF log line ("\033[0;33mW (1234) wifi: text\033[0m\n") ---- */

typedef struct {
    char lvl;           /* E W I D V */
    const char *tag;
    size_t tag_len;
    const char *msg;
    size_t msg_len;
} dl_log_line_t;

/**
 * @brief Split a log line into level, tag and message (colour codes and the trailing newline removed).
 *        Lines that do not look like ESP_LOG output come back as level 'I', empty tag, whole line as message.
 *
 * @requirement SPEC-5.7-LOG
 * @parent      -
 * @test        host:stream_log_*
 */
void dl_log_parse(const char *line, size_t len, dl_log_line_t *out);

#ifdef __cplusplus
}
#endif
