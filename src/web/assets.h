#ifndef UC_ASSETS_H
#define UC_ASSETS_H
#include <stddef.h>
struct uc_asset {
    const char *path;
    const char *type;
    const char *body;
};
extern const struct uc_asset uc_assets[];
extern const size_t uc_assets_count;
#endif
