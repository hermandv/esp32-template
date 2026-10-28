/*
 * devlink recovery image (SPEC §6): devlink core only. Wi-Fi with an always-on config AP, Improv over USB,
 * HTTP API subset with OTA, discovery, captive DNS, health task feeding the RTC WDT.
 */
#include <stdio.h>
#include <stdlib.h>

#include "devlink.h"
#include "sdkconfig.h"
#if CONFIG_DEVLINK_TEST_CONSOLE
#include "devlink_testcon.h"
#endif

void app_main(void)
{
    devlink_boot_early(DEVLINK_MODE_RECOVERY);

#if CONFIG_DEVLINK_TEST_RECOVERY_CRASH
    printf("DLT recovery TEST BUILD: crashing at start\n");
    fflush(stdout);
    abort();
#endif

#if CONFIG_DEVLINK_TEST_CONSOLE
    devlink_testcon_start("recovery", NULL); /* test builds: devlink console + HIL test commands */
#else
    devlink_console_start(NULL);
#endif
    devlink_start(DEVLINK_MODE_RECOVERY);
    /* Recovery's self-test is the health task: it keeps the RTC WDT fed only while HTTP and Wi-Fi run. */
    devlink_mark_valid();
}
