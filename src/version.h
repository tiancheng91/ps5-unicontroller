#ifndef UC_VERSION_H
#define UC_VERSION_H
#ifdef UC_BUILD_METADATA_HEADER
#include "build_info.h"
#endif
#ifndef UC_VERSION
#define UC_VERSION "dev"
#endif
#ifndef UC_BUILD_METADATA_JSON
#define UC_BUILD_METADATA_JSON                                                                     \
    "\"version\":\"" UC_VERSION "\",\"build_id\":\"" UC_VERSION                                    \
    "\",\"commit\":\"unknown\",\"branch\":\"local\""
#endif
#endif
