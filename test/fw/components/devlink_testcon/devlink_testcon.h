/* Test console for milestone-1 HIL tests. TEST BUILDS ONLY. */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Extra command handler of the image; return true if handled. @p arg may be NULL. */
typedef bool (*devlink_testcon_cmd_fn)(const char *cmd, const char *arg);

/** Start the console task and print "DLT ready" + "DLT info". */
void devlink_testcon_start(const char *image, devlink_testcon_cmd_fn extra);

#ifdef __cplusplus
}
#endif
