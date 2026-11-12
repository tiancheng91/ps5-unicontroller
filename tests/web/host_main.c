#include "../../src/web/runtime.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv)
{
    unsigned long port = argc > 1 ? strtoul(argv[1], NULL, 10) : 7456;
    if (port == 0 || port > 65535)
        return 2;
    if (uc_backend_init())
        return 1;
    return uc_runtime_run(argc > 3 ? argv[3] : "127.0.0.1", (unsigned short)port,
                          argc > 2 ? argv[2] : "unicontroller-settings.json")
               ? 1
               : 0;
}
