#include "../../src/web/runtime.h"
#include "../../src/logging.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv)
{
    unsigned long port = argc > 1 ? strtoul(argv[1], NULL, 10) : 7456;
    if (port == 0 || port > 65535)
        return 2;
    if (uc_backend_init())
        return 1;
    const char *settings_path = argc > 2 ? argv[2] : "unicontroller-settings.json";
    char log_path[1024];
    int length = snprintf(log_path, sizeof log_path, "%s.runtime.log", settings_path);
    if (length < 0 || (size_t)length >= sizeof log_path || !uc_log_open(log_path))
        return 1;
    int result =
        uc_runtime_run(argc > 3 ? argv[3] : "127.0.0.1", (unsigned short)port, settings_path);
    uc_log_close();
    return result ? 1 : 0;
}
