#ifndef UC_RUNTIME_H
#define UC_RUNTIME_H
#include <stdint.h>
int uc_runtime_run(const char *bind_address, unsigned short port, const char *settings_path);
#include "../core/uc_core.h"
#include "../ps5/uc_backend.h"
extern const char uc_index_html[];
extern const char uc_controller_html[];
extern const char uc_settings_html[];
#endif
