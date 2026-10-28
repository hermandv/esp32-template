/*
 * Button logic (SPEC §5.9). Pure C, see dl_button.h.
 */
#include "dl_button.h"

#include <string.h>

void dl_button_init(dl_button_t *b, bool pressed_at_boot)
{
    memset(b, 0, sizeof(*b));
    b->stuck = pressed_at_boot;
}

dl_btn_action_t dl_button_step(dl_button_t *b, bool pressed, uint64_t now)
{
    if (b->stuck) {
        if (!pressed) {
            b->stuck = false; /* released: from now on it counts */
        }
        return DL_BTN_NONE;
    }
    if (!pressed) {
        b->pressed = false;
        return DL_BTN_NONE;
    }
    if (!b->pressed) {
        b->pressed = true;
        b->since = now;
        b->ap_done = false;
        b->recovery_done = false;
        return DL_BTN_NONE;
    }
    uint64_t held = now - b->since;
    if ((held >= DL_BUTTON_RECOVERY_MS) && !b->recovery_done) {
        b->recovery_done = true;
        return DL_BTN_RECOVERY;
    }
    if ((held >= DL_BUTTON_AP_MS) && !b->ap_done) {
        b->ap_done = true;
        return DL_BTN_AP;
    }
    return DL_BTN_NONE;
}
