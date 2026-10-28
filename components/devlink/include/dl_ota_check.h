/*
 * OTA image checks (SPEC §7.1 step 3, acceptance test 21). Pure C.
 *
 * Feed the upload stream as it arrives. The first DL_OTA_HEAD_LEN bytes are buffered and checked before the caller
 * writes anything (image header, chip, revision, flash size, app descriptor, project prefix). The devlink marker is
 * searched over the whole stream and checked at the end (the slot is only activated after that).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Image layout (ESP-IDF esp_image_header_t, esp_image_segment_header_t, esp_app_desc_t). */
#define DL_IMG_HEADER_LEN   24u
#define DL_IMG_SEG_HDR_LEN  8u
#define DL_APP_DESC_LEN     256u
#define DL_OTA_HEAD_LEN     (DL_IMG_HEADER_LEN + DL_IMG_SEG_HDR_LEN + DL_APP_DESC_LEN)
#define DL_IMG_MAGIC        0xE9u
#define DL_APP_DESC_MAGIC   0xABCD5432u
#define DL_CHIP_ID_ESP32S3  9u

/* devlink marker: a constant struct every devlink build contains (components/devlink/dl_marker.c). */
/* 16 bytes, no terminator. 0xD1 occurs only as the first byte (the stream matcher relies on it). */
#define DL_MARKER_MAGIC_BYTES \
    {0xD1, 'd', 'e', 'v', 'l', 'i', 'n', 'k', '-', 'm', 'a', 'r', 'k', 'e', 'r', 0x01}
#define DL_MARKER_MAGIC_LEN 16u
#define DEVLINK_API_VERSION 1u

typedef struct {
    uint8_t magic[DL_MARKER_MAGIC_LEN];
    uint32_t api_version;
    uint32_t reserved[3];
} dl_marker_t;

typedef enum {
    DL_OTA_OK = 0,
    DL_OTA_NEED_MORE,      /* header not complete yet: keep feeding, write nothing */
    DL_OTA_E_SIZE,         /* Content-Length missing, too small, or larger than the target partition */
    DL_OTA_E_MAGIC,        /* not an ESP image (ELF, random file) */
    DL_OTA_E_CHIP,         /* image for another chip */
    DL_OTA_E_CHIP_REV,     /* chip revision outside the image's min/max */
    DL_OTA_E_NOT_APP,      /* no app descriptor: bootloader or merged image */
    DL_OTA_E_FLASH_SIZE,   /* image built for a larger flash */
    DL_OTA_E_PROJECT,      /* project name prefix mismatch (force=1 overrides) */
    DL_OTA_E_NO_MARKER,    /* not a devlink build (force=1 overrides) */
    DL_OTA_E_LENGTH,       /* more or fewer bytes than announced */
} dl_ota_result_t;

typedef struct {
    uint16_t chip_id;         /* DL_CHIP_ID_ESP32S3 */
    uint16_t chip_rev_full;   /* this chip: major * 100 + minor */
    uint32_t flash_size;      /* this device's flash, bytes */
    uint32_t partition_size;  /* target partition, bytes */
    const char *project_prefix; /* "" or NULL = no check */
    bool force;
} dl_ota_expect_t;

typedef struct {
    dl_ota_expect_t exp;
    uint32_t total;           /* announced length */
    uint32_t received;
    uint8_t head[DL_OTA_HEAD_LEN];
    size_t head_len;
    bool head_checked;
    size_t marker_match;      /* bytes of the marker magic matched so far */
    bool marker_found;
    /* From the app descriptor, for the reply and the PC app. */
    char version[33];
    char project[33];
    char date[17];
    char time[17];
    char idf_ver[33];
} dl_ota_check_t;

/**
 * @brief Start checking an upload of @p total bytes.
 * @return DL_OTA_OK, or DL_OTA_E_SIZE (reject before reading the body).
 *
 * @requirement SPEC-7.1
 * @parent      I4, I1
 * @test        host:ota_*, T21
 */
dl_ota_result_t dl_ota_check_begin(dl_ota_check_t *c, const dl_ota_expect_t *exp, uint32_t total);

/**
 * @brief Feed the next bytes. Returns DL_OTA_NEED_MORE while the header is incomplete, an error code if the header
 *        is rejected, otherwise DL_OTA_OK. When the header has just been accepted, @p *flush_head is set: the caller
 *        must first write c->head (c->head_len bytes), then this chunk's remaining bytes from @p data + @p *data_off.
 *        Afterwards each call returns DL_OTA_OK with *data_off = 0.
 */
dl_ota_result_t dl_ota_check_feed(dl_ota_check_t *c, const uint8_t *data, size_t len, bool *flush_head,
                                  size_t *data_off);

/** All bytes received: final verdict (length, marker). */
dl_ota_result_t dl_ota_check_finish(const dl_ota_check_t *c);

/** Short machine-readable name of a result ("ok", "wrong_chip", ...). */
const char *dl_ota_result_name(dl_ota_result_t r);

#ifdef __cplusplus
}
#endif
