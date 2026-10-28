/*
 * Device identity (SPEC §4) and the devlink marker (SPEC §7.1).
 */
#include <stdio.h>
#include <string.h>

#include "dl_ota_check.h"
#include "dl_priv.h"
#include "esp_mac.h"
#include "sdkconfig.h"

/* Every devlink image carries this marker; uploads without it are rejected unless force=1 (SPEC §7.1). */
__attribute__((used)) const dl_marker_t devlink_marker = {
    .magic = DL_MARKER_MAGIC_BYTES,
    .api_version = DEVLINK_API_VERSION,
    .reserved = {0, 0, 0},
};

static dl_ident_t s_id;

void dl_ident_init(bool recovery)
{
    (void)esp_read_mac(s_id.mac, ESP_MAC_BASE);
    snprintf(s_id.id, sizeof(s_id.id), "%02X%02X%02X", s_id.mac[3], s_id.mac[4], s_id.mac[5]);
    snprintf(s_id.mac_str, sizeof(s_id.mac_str), "%02X:%02X:%02X:%02X:%02X:%02X", s_id.mac[0], s_id.mac[1],
             s_id.mac[2], s_id.mac[3], s_id.mac[4], s_id.mac[5]);
    snprintf(s_id.hostname, sizeof(s_id.hostname), "esp-%02x%02x%02x", s_id.mac[3], s_id.mac[4], s_id.mac[5]);
    s_id.board = CONFIG_DEVLINK_BOARD;
    s_id.mode = recovery ? "recovery" : "app";
}

const dl_ident_t *dl_ident(void)
{
    return &s_id;
}

void dl_ident_set_label(const char *label)
{
    strncpy(s_id.label, label, sizeof(s_id.label) - 1u);
    s_id.label[sizeof(s_id.label) - 1u] = '\0';
}
