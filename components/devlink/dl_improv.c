/*
 * Improv Wi-Fi serial protocol v1 (SPEC §5.4). Pure C, see dl_improv.h.
 */
#include "dl_improv.h"

#include <string.h>

static const uint8_t k_prefix[6] = {'I', 'M', 'P', 'R', 'O', 'V'};

void dl_improv_rx_init(dl_improv_rx_state_t *s)
{
    memset(s, 0, sizeof(*s));
}

static uint8_t sum(const uint8_t *p, size_t n)
{
    uint8_t c = 0;
    for (size_t i = 0; i < n; i++) {
        c = (uint8_t)(c + p[i]);
    }
    return c;
}

dl_improv_rx_t dl_improv_rx_byte(dl_improv_rx_state_t *s, uint8_t b)
{
    s->pass_len = 0;

    if (s->len < sizeof(k_prefix)) {
        if (b == k_prefix[s->len]) {
            s->buf[s->len++] = b;
            return DL_IMPROV_RX_MORE;
        }
        /* Not a frame after all: release what was held back plus this byte. A new 'I' restarts matching. */
        memcpy(s->pass, s->buf, s->len);
        s->pass_len = s->len;
        s->len = 0;
        if (b == k_prefix[0]) {
            s->buf[s->len++] = b;
        } else {
            s->pass[s->pass_len++] = b;
        }
        return (s->pass_len > 0u) ? DL_IMPROV_RX_PASS : DL_IMPROV_RX_MORE;
    }

    s->buf[s->len++] = b;
    if (s->len < 9u) {
        return DL_IMPROV_RX_MORE; /* version, type, length */
    }
    size_t data_len = s->buf[8];
    if (s->len < 9u + data_len + 1u) {
        return DL_IMPROV_RX_MORE;
    }

    /* Complete. */
    size_t total = s->len;
    s->len = 0;
    if ((s->buf[6] != DL_IMPROV_VERSION) || (sum(s->buf, total - 1u) != s->buf[total - 1u])) {
        return DL_IMPROV_RX_BAD;
    }
    s->type = s->buf[7];
    s->data = &s->buf[9];
    s->data_len = data_len;
    return DL_IMPROV_RX_FRAME;
}

size_t dl_improv_frame(uint8_t type, const uint8_t *data, size_t len, uint8_t *out)
{
    if (len > DL_IMPROV_MAX_DATA) {
        return 0;
    }
    memcpy(out, k_prefix, sizeof(k_prefix));
    out[6] = DL_IMPROV_VERSION;
    out[7] = type;
    out[8] = (uint8_t)len;
    if (len > 0u) {
        memcpy(out + 9, data, len);
    }
    out[9 + len] = sum(out, 9 + len);
    return 10 + len;
}

size_t dl_improv_rpc_result(uint8_t cmd, const char *const *strings, size_t n, uint8_t *out)
{
    uint8_t data[DL_IMPROV_MAX_DATA];
    size_t len = 2;

    data[0] = cmd;
    for (size_t i = 0; i < n; i++) {
        size_t sl = strlen(strings[i]);
        if ((sl > 255u) || (len + 1u + sl > sizeof(data))) {
            return 0;
        }
        data[len++] = (uint8_t)sl;
        memcpy(data + len, strings[i], sl);
        len += sl;
    }
    data[1] = (uint8_t)(len - 2u);
    return dl_improv_frame(DL_IMPROV_T_RPC_RESULT, data, len, out);
}

bool dl_improv_parse_rpc(const uint8_t *data, size_t len, uint8_t *cmd, const uint8_t **payload, size_t *payload_len)
{
    if ((len < 2u) || ((size_t)data[1] + 2u != len)) {
        return false;
    }
    *cmd = data[0];
    *payload = data + 2;
    *payload_len = data[1];
    return true;
}

bool dl_improv_parse_wifi(const uint8_t *payload, size_t len, char ssid[33], char password[65])
{
    if (len < 1u) {
        return false;
    }
    size_t sl = payload[0];
    if ((sl == 0u) || (sl > 32u) || (1u + sl + 1u > len)) {
        return false;
    }
    size_t pl = payload[1u + sl];
    if ((pl > 64u) || (1u + sl + 1u + pl != len)) {
        return false;
    }
    memcpy(ssid, payload + 1, sl);
    ssid[sl] = '\0';
    memcpy(password, payload + 2u + sl, pl);
    password[pl] = '\0';
    /* No NULs inside the strings. */
    return (strlen(ssid) == sl) && (strlen(password) == pl);
}
