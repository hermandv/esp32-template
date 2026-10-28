/*
 * Button logic (SPEC §5.9). Pure C: the glue samples the pin and calls dl_button_step().
 *   hold 3 s  -> config AP (once per press)
 *   hold 10 s -> reboot into recovery (one-shot flag)
 *   pressed at boot -> treated as stuck, ignored until released
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DL_BUTTON_AP_MS       3000u
#define DL_BUTTON_RECOVERY_MS 10000u

typedef enum {
    DL_BTN_NONE = 0,
    DL_BTN_AP,
    DL_BTN_RECOVERY,
} dl_btn_action_t;

typedef struct {
    bool stuck;
    bool pressed;
    uint64_t since;
    bool ap_done;
    bool recovery_done;
} dl_button_t;

void dl_button_init(dl_button_t *b, bool pressed_at_boot);

/**
 * @brief Feed one sample (pressed = active level) at time @p now (ms).
 *
 * @requirement SPEC-5.9
 * @parent      I2, I6
 * @test        host:button_*, T14
 */
dl_btn_action_t dl_button_step(dl_button_t *b, bool pressed, uint64_t now);

#ifdef __cplusplus
}
#endif
