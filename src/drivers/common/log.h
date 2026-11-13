#ifndef UC_DRIVER_LOG_H
#define UC_DRIVER_LOG_H
#include "../../logging.h"
/* Compatibility entry points for existing transport/protocol diagnostics.
 * They use the same rxi logger, output and threshold as the rest of the app. */
#define log_line(...) uc_log_info(__VA_ARGS__)
#define log_open(path) uc_log_open(path)
#define log_close() uc_log_close()
#endif
