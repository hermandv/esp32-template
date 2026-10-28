/*
 * devlink recovery image, milestone 1 (SPEC §6): devlink boot steps and the test console.
 * Wi-Fi, provisioning, API and OTA follow in milestone 2.
 */
#include <stdio.h>
#include <stdlib.h>

#include "devlink.h"
#include "devlink_testcon.h"
#include "sdkconfig.h"

void app_main(void)
{
    devlink_boot_early(DEVLINK_MODE_RECOVERY);

#if CONFIG_DEVLINK_TEST_RECOVERY_CRASH
    printf("DLT recovery TEST BUILD: crashing at start\n");
    fflush(stdout);
    abort();
#endif

    devlink_testcon_start("recovery", NULL);
    /* Self-test placeholder: the console is up. Milestone 2: AP/STA + API listening. */
    devlink_mark_valid();
}
