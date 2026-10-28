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
