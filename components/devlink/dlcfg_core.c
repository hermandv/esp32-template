/*
 * devlink config record format (SPEC §5.3). Pure C, see dlcfg_core.h.
 */
#include "dlcfg_core.h"

#include <string.h>

#include "dl_rtc.h" /* dl_crc32 */

static uint32_t record_crc(const dlcfg_header_t *hdr, const uint8_t *payload)
{
    /* CRC-32 over the header bytes before crc, then the payload. */
    uint32_t state = dl_crc32_update(DL_CRC32_INIT, hdr, (uint32_t)offsetof(dlcfg_header_t, crc));
    state = dl_crc32_update(state, payload, hdr->payload_len);
    return ~state;
}

void dlcfg_defaults(dlcfg_t *cfg)
{
    (void)memset(cfg, 0, sizeof(*cfg));
    cfg->log_level = 3u; /* info */
    cfg->ap_window_s = 20u;
    cfg->button_window_s = 120u;
}

bool dlcfg_parse(const uint8_t *sector, size_t len, dlcfg_t *cfg, uint32_t *seq)
{
    dlcfg_header_t hdr;

    if (len < sizeof(hdr)) {
        return false;
    }
    (void)memcpy(&hdr, sector, sizeof(hdr));
    if ((hdr.magic != DLCFG_MAGIC) || (hdr.format_version == 0u) || (hdr.payload_len == 0u) ||
        ((size_t)hdr.payload_len > (len - sizeof(hdr)))) {
        return false;
    }
    if (record_crc(&hdr, sector + sizeof(hdr)) != hdr.crc) {
        return false;
    }

    dlcfg_defaults(cfg);
    size_t n = (hdr.payload_len < sizeof(*cfg)) ? hdr.payload_len : sizeof(*cfg);
    (void)memcpy(cfg, sector + sizeof(hdr), n);

    /* Never trust string termination from flash. */
    cfg->wifi_ssid[sizeof(cfg->wifi_ssid) - 1u] = '\0';
    cfg->wifi_pass[sizeof(cfg->wifi_pass) - 1u] = '\0';
    cfg->prev_ssid[sizeof(cfg->prev_ssid) - 1u] = '\0';
    cfg->prev_pass[sizeof(cfg->prev_pass) - 1u] = '\0';
    cfg->label[sizeof(cfg->label) - 1u] = '\0';
    if (cfg->reset_ring_pos >= DLCFG_RESET_RING) {
        cfg->reset_ring_pos = 0u;
    }
    *seq = hdr.sequence;
    return true;
}

int dlcfg_select(const uint8_t *s0, const uint8_t *s1, size_t len, dlcfg_t *cfg, uint32_t *seq)
{
    dlcfg_t c0;
    dlcfg_t c1;
    uint32_t q0 = 0u;
    uint32_t q1 = 0u;
    bool v0 = dlcfg_parse(s0, len, &c0, &q0);
    bool v1 = dlcfg_parse(s1, len, &c1, &q1);
    int sel;

    if (v0 && v1) {
        sel = ((int32_t)(q1 - q0) > 0) ? 1 : 0; /* wrap-safe */
    } else if (v0) {
        sel = 0;
    } else if (v1) {
        sel = 1;
    } else {
        dlcfg_defaults(cfg);
        *seq = 0u;
        return -1;
    }
    *cfg = (sel == 0) ? c0 : c1;
    *seq = (sel == 0) ? q0 : q1;
    return sel;
}

size_t dlcfg_build(const dlcfg_t *cfg, uint32_t seq, uint8_t *buf, size_t buflen)
{
    dlcfg_header_t hdr = {
        .magic = DLCFG_MAGIC,
        .format_version = DLCFG_FORMAT_VERSION,
        .payload_len = (uint16_t)sizeof(*cfg),
        .sequence = seq,
        .crc = 0u,
    };

    if (buflen < dlcfg_record_size()) {
        return 0u;
    }
    (void)memcpy(buf + sizeof(hdr), cfg, sizeof(*cfg));
    hdr.crc = record_crc(&hdr, buf + sizeof(hdr));
    (void)memcpy(buf, &hdr, sizeof(hdr));
    return dlcfg_record_size();
}

void dlcfg_push_reset_reason(dlcfg_t *cfg, uint32_t reason)
{
    cfg->reset_ring[cfg->reset_ring_pos % DLCFG_RESET_RING] = reason;
    cfg->reset_ring_pos = (uint8_t)((cfg->reset_ring_pos + 1u) % DLCFG_RESET_RING);
}
