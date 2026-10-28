/*
 * Improv Wi-Fi serial protocol, version 1 (https://www.improv-wifi.com/serial/), SPEC §5.4. Pure C.
 *
 * Frame: "IMPROV" | version (1) | type (1) | length (1) | data[length] | checksum (1, sum of all previous bytes).
 * The receiver shares the console with text lines: bytes that turn out not to be part of a frame are handed back
 * for the line handler.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DL_IMPROV_VERSION 1u
#define DL_IMPROV_MAX_DATA 255u
#define DL_IMPROV_MAX_FRAME (6u + 3u + DL_IMPROV_MAX_DATA + 1u)

/* Frame types */
#define DL_IMPROV_T_STATE      0x01u
#define DL_IMPROV_T_ERROR      0x02u
#define DL_IMPROV_T_RPC        0x03u
#define DL_IMPROV_T_RPC_RESULT 0x04u

/* States */
#define DL_IMPROV_S_READY        0x02u /* "authorized" */
#define DL_IMPROV_S_PROVISIONING 0x03u
#define DL_IMPROV_S_PROVISIONED  0x04u

/* Errors */
#define DL_IMPROV_E_NONE        0x00u
#define DL_IMPROV_E_INVALID_RPC 0x01u
#define DL_IMPROV_E_UNKNOWN_RPC 0x02u
#define DL_IMPROV_E_CONNECT     0x03u
#define DL_IMPROV_E_UNKNOWN     0xFFu

/* RPC commands */
#define DL_IMPROV_CMD_WIFI     0x01u
#define DL_IMPROV_CMD_STATE    0x02u
#define DL_IMPROV_CMD_INFO     0x03u
#define DL_IMPROV_CMD_SCAN     0x04u

typedef enum {
    DL_IMPROV_RX_MORE = 0,   /* byte consumed, nothing to do yet */
    DL_IMPROV_RX_PASS,       /* not a frame: hand pass[0..pass_len) to the line handler */
    DL_IMPROV_RX_FRAME,      /* complete frame with a valid checksum: type/data/len */
    DL_IMPROV_RX_BAD,        /* complete frame, wrong checksum or version */
} dl_improv_rx_t;

typedef struct {
    uint8_t buf[DL_IMPROV_MAX_FRAME];
    size_t len;
    uint8_t pass[8];         /* bytes released back to the line handler */
    size_t pass_len;
    uint8_t type;
    const uint8_t *data;     /* valid after RX_FRAME until the next call */
    size_t data_len;
} dl_improv_rx_state_t;

void dl_improv_rx_init(dl_improv_rx_state_t *s);

/**
 * @brief Feed one received byte.
 *
 * @requirement SPEC-5.4-IMPROV
 * @parent      I6
 * @test        host:improv_*, T16a
 */
dl_improv_rx_t dl_improv_rx_byte(dl_improv_rx_state_t *s, uint8_t b);

/** Build a frame into @p out (>= DL_IMPROV_MAX_FRAME). Returns its length. */
size_t dl_improv_frame(uint8_t type, const uint8_t *data, size_t len, uint8_t *out);

/** Build an RPC result frame: command + list of strings (each length-prefixed). Returns length, 0 if too long. */
size_t dl_improv_rpc_result(uint8_t cmd, const char *const *strings, size_t n, uint8_t *out);

/** Parse an RPC frame's data: command and its payload. False if malformed. */
bool dl_improv_parse_rpc(const uint8_t *data, size_t len, uint8_t *cmd, const uint8_t **payload, size_t *payload_len);

/** Parse the Wi-Fi settings payload into NUL-terminated ssid (<= 32) / password (<= 64). False if malformed. */
bool dl_improv_parse_wifi(const uint8_t *payload, size_t len, char ssid[33], char password[65]);

#ifdef __cplusplus
}
#endif
