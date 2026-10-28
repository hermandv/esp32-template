/*
 * OTA image checks (SPEC §7.1). Pure C, see dl_ota_check.h.
 */
#include "dl_ota_check.h"

#include <string.h>

static const uint8_t k_marker[DL_MARKER_MAGIC_LEN] = DL_MARKER_MAGIC_BYTES;

/* Header field offsets (esp_image_header_t, packed). */
#define OFF_SPI_SIZE   3u  /* high nibble */
#define OFF_CHIP_ID    12u
#define OFF_MIN_REV    15u
#define OFF_MAX_REV    17u
/* esp_app_desc_t offsets, relative to the descriptor. */
#define DESC_OFF       (DL_IMG_HEADER_LEN + DL_IMG_SEG_HDR_LEN)
#define D_VERSION      16u
#define D_PROJECT      48u
#define D_TIME         80u
#define D_DATE         96u
#define D_IDF          112u

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void copy_field(char *dst, size_t dst_size, const uint8_t *src, size_t src_len)
{
    size_t n = 0;
    while ((n < src_len) && (n + 1u < dst_size) && (src[n] != 0u)) {
        dst[n] = (char)src[n];
        n++;
    }
    dst[n] = '\0';
}

dl_ota_result_t dl_ota_check_begin(dl_ota_check_t *c, const dl_ota_expect_t *exp, uint32_t total)
{
    memset(c, 0, sizeof(*c));
    c->exp = *exp;
    c->total = total;
    if ((total < DL_OTA_HEAD_LEN) || (total > exp->partition_size)) {
        return DL_OTA_E_SIZE;
    }
    return DL_OTA_OK;
}

static void scan_marker(dl_ota_check_t *c, const uint8_t *data, size_t len)
{
    for (size_t i = 0; (i < len) && !c->marker_found; i++) {
        uint8_t b = data[i];
        if (k_marker[c->marker_match] == b) {
            c->marker_match++;
        } else {
            /* The magic's first byte (0xD1) occurs nowhere else in it, so restarting is exact. */
            c->marker_match = (k_marker[0] == b) ? 1u : 0u;
        }
        if (c->marker_match == DL_MARKER_MAGIC_LEN) {
            c->marker_found = true;
        }
    }
}

static dl_ota_result_t check_head(dl_ota_check_t *c)
{
    const uint8_t *h = c->head;
    const uint8_t *d = h + DESC_OFF;

    if (h[0] != DL_IMG_MAGIC) {
        return DL_OTA_E_MAGIC;
    }
    if (rd16(h + OFF_CHIP_ID) != c->exp.chip_id) {
        return DL_OTA_E_CHIP;
    }
    uint16_t min_rev = rd16(h + OFF_MIN_REV);
    uint16_t max_rev = rd16(h + OFF_MAX_REV);
    if ((c->exp.chip_rev_full < min_rev) || (c->exp.chip_rev_full > max_rev)) {
        return DL_OTA_E_CHIP_REV;
    }
    if (rd32(d) != DL_APP_DESC_MAGIC) {
        return DL_OTA_E_NOT_APP;
    }
    uint32_t flash = (1024u * 1024u) << (h[OFF_SPI_SIZE] >> 4);
    if (flash > c->exp.flash_size) {
        return DL_OTA_E_FLASH_SIZE;
    }

    copy_field(c->version, sizeof(c->version), d + D_VERSION, 32u);
    copy_field(c->project, sizeof(c->project), d + D_PROJECT, 32u);
    copy_field(c->time, sizeof(c->time), d + D_TIME, 16u);
    copy_field(c->date, sizeof(c->date), d + D_DATE, 16u);
    copy_field(c->idf_ver, sizeof(c->idf_ver), d + D_IDF, 32u);

    const char *prefix = c->exp.project_prefix;
    if (!c->exp.force && (prefix != NULL) && (prefix[0] != '\0') &&
        (strncmp(c->project, prefix, strlen(prefix)) != 0)) {
        return DL_OTA_E_PROJECT;
    }
    return DL_OTA_OK;
}

dl_ota_result_t dl_ota_check_feed(dl_ota_check_t *c, const uint8_t *data, size_t len, bool *flush_head,
                                  size_t *data_off)
{
    *flush_head = false;
    *data_off = 0;

    if ((uint64_t)c->received + len > c->total) {
        return DL_OTA_E_LENGTH;
    }
    c->received += (uint32_t)len;
    scan_marker(c, data, len);

    if (c->head_checked) {
        return DL_OTA_OK;
    }
    size_t take = DL_OTA_HEAD_LEN - c->head_len;
    if (take > len) {
        take = len;
    }
    memcpy(c->head + c->head_len, data, take);
    c->head_len += take;
    if (c->head_len < DL_OTA_HEAD_LEN) {
        return DL_OTA_NEED_MORE;
    }
    dl_ota_result_t r = check_head(c);
    if (r != DL_OTA_OK) {
        return r;
    }
    c->head_checked = true;
    *flush_head = true;
    *data_off = take;
    return DL_OTA_OK;
}

dl_ota_result_t dl_ota_check_finish(const dl_ota_check_t *c)
{
    if ((c->received != c->total) || !c->head_checked) {
        return DL_OTA_E_LENGTH;
    }
    if (!c->marker_found && !c->exp.force) {
        return DL_OTA_E_NO_MARKER;
    }
    return DL_OTA_OK;
}

const char *dl_ota_result_name(dl_ota_result_t r)
{
    switch (r) {
    case DL_OTA_OK:
        return "ok";
    case DL_OTA_NEED_MORE:
        return "need_more";
    case DL_OTA_E_SIZE:
        return "bad_size";
    case DL_OTA_E_MAGIC:
        return "not_an_esp_image";
    case DL_OTA_E_CHIP:
        return "wrong_chip";
    case DL_OTA_E_CHIP_REV:
        return "wrong_chip_revision";
    case DL_OTA_E_NOT_APP:
        return "not_an_app_image";
    case DL_OTA_E_FLASH_SIZE:
        return "flash_too_small";
    case DL_OTA_E_PROJECT:
        return "wrong_project";
    case DL_OTA_E_NO_MARKER:
        return "not_a_devlink_image";
    case DL_OTA_E_LENGTH:
        return "length_mismatch";
    default:
        return "unknown";
    }
}
