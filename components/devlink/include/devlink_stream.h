/*
 * devlink module API (SPEC §5.7, §5.8): stream channels that test modules publish to. Clients subscribe over the
 * WebSocket /ws and choose a rate per channel; slow clients lose frames (counted), never memory.
 *
 *   devlink_channel_t *ch = devlink_channel_register(&(devlink_channel_cfg_t){
 *       .name = "ina219.bus_v", .unit = "V", .type = DL_NUM, .default_hz = 10, .max_hz = 200});
 *   devlink_publish_num(ch, value);
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DL_NUM = 0, /* one number:          {"ch":..,"ts":..,"v":41.5} */
    DL_ARR = 1, /* array of numbers:    {"ch":..,"ts":..,"v":[1,2,3]} */
    DL_OBJ = 2, /* any JSON value:      {"ch":..,"ts":..,"v":{...}} */
    DL_BLOCK = 3, /* binary blocks of raw samples: devlink_block_register / devlink_publish_block */
} devlink_type_t;

typedef struct {
    const char *name;      /* "module.signal", <= 31 chars */
    const char *unit;      /* "V", "degC", "" ... <= 11 chars */
    devlink_type_t type;
    uint16_t default_hz;   /* rate a new subscription gets; 0 = every published value (events) */
    uint16_t max_hz;       /* highest rate a client may ask for; 0 = no limit */
} devlink_channel_cfg_t;

typedef struct devlink_channel devlink_channel_t;

#define DEVLINK_MAX_CHANNELS 32
#define DEVLINK_MAX_WS_CLIENTS 2

/**
 * @brief Register a channel (name must be unique). Returns NULL if the table is full or the name is taken.
 *
 * @requirement SPEC-5.8
 * @parent      -
 * @test        T3
 */
devlink_channel_t *devlink_channel_register(const devlink_channel_cfg_t *cfg);

/** Find a registered channel by name (NULL if none). */
devlink_channel_t *devlink_channel_find(const char *name);

/**
 * @brief Publish a value. Cheap when nobody listens; rate-limited per client; paused during an OTA (SPEC §5.7).
 *
 * @requirement SPEC-5.7
 * @parent      -
 * @test        T3
 */
void devlink_publish_num(devlink_channel_t *ch, double value);
void devlink_publish_arr(devlink_channel_t *ch, const double *values, size_t n);
/** @p json must be a valid JSON value. */
void devlink_publish_json(devlink_channel_t *ch, const char *json);

/* ---- Block channels (SPEC §5.7): high-rate raw samples, sent as binary WebSocket messages on /ws ----
 * Register once with devlink_block_register(); collect samples into blocks of about 50 ms and publish each block
 * with devlink_publish_block(). devlink_channel_wanted_hz() is the sample rate to run at: the highest rate any
 * subscriber asked for (every subscriber gets every block). Off (register returns NULL) when
 * CONFIG_DEVLINK_BLOCK_SLOTS is 0, as in recovery. The binary layout is in dl_stream_core.h (DL_BLOCK_*). */

#define DEVLINK_BLOCK_MAX_FIELDS 8
#define DEVLINK_BLOCK_FIELDS_MAX 63 /* length of the field-name string */
#define DEVLINK_BLOCK_MAX_BYTES  1300 /* sample bytes per block: n_samples * n_fields * sample size */

typedef enum { DEVLINK_SAMPLE_I16 = 0, DEVLINK_SAMPLE_I32 = 1, DEVLINK_SAMPLE_F32 = 2 } devlink_sample_t;

typedef struct {
    const char *fields;      /* field names, comma-separated: "ax,ay,az" (<= 63 chars; NULL = unnamed). Kept by
                                reference: must stay valid (a string literal) */
    uint8_t n_fields;        /* 1..DEVLINK_BLOCK_MAX_FIELDS; must match the names */
    devlink_sample_t sample;
} devlink_block_meta_t;

/**
 * @brief Register a block channel (cfg->type is ignored). NULL if the table is full, the meta is invalid or block
 *        channels are off in this build.
 *
 * @requirement SPEC-5.7-BLOCK
 * @parent      -
 * @test        host:stream_block_*, T27
 */
devlink_channel_t *devlink_block_register(const devlink_channel_cfg_t *cfg, const devlink_block_meta_t *meta);

/**
 * @brief Queue one block for every subscriber: @p n_samples x n_fields samples, interleaved, little-endian, with the
 *        time of the first sample (µs since boot, as esp_timer_get_time), the sample period, and one scale per field
 *        (value = raw * scale; NULL = 1.0). @p flags: 0, or DEVLINK_BLOCK_SRC_GAP when the producer lost samples
 *        before this block. Cheap when nobody listens; paused during an OTA. A subscriber whose ring is full loses
 *        the block (counted; its next block carries the GAP flag and the sequence number shows the gap).
 *
 * @requirement SPEC-5.7-BLOCK
 * @parent      -
 * @test        T27
 */
void devlink_publish_block(devlink_channel_t *ch, int64_t t0_us, uint32_t period_ns, const float *scale,
                           const void *samples, uint16_t n_samples, uint8_t flags);

#define DEVLINK_BLOCK_SRC_GAP 0x02u

/** Highest rate any client wants for @p ch (Hz); 0 = nobody subscribed. Producers can skip sampling when 0.
 *  An event-type subscription (rate 0) reports the channel's max_hz (or 1000 if unlimited). */
uint32_t devlink_channel_wanted_hz(const devlink_channel_t *ch);

/**
 * @brief printf-style message on the debug channel "dbg.<ch>" (registered on first use).
 *
 * @requirement SPEC-5.7
 * @parent      -
 * @test        T3
 */
void devlink_debug(const char *ch, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/** Deep sleep that refuses to sleep without a timer wake-up (SPEC §5.8 module rules). */
esp_err_t devlink_sleep(uint32_t seconds);

/** Last die temperature for /status (set by devlink_sysmon). */
void devlink_set_temperature(float celsius);

#ifdef __cplusplus
}
#endif
