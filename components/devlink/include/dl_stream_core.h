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
