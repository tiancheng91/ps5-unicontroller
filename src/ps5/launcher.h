#ifndef UC_LAUNCHER_H
#define UC_LAUNCHER_H
#include <stddef.h>
#include <stdint.h>
#define UC_LAUNCHER_TITLE_ID "UNIC00001"
/* Write only our shortcut files. 1 changed, 0 unchanged, -1 failed. */
int uc_launcher_write(const char *root, unsigned short port, const unsigned char *icon,
                      size_t size);
/* PS5 only; failures never disable the HTTP server or physical inputs. */
int uc_launcher_install(unsigned short port, int force);
int uc_launcher_remove(void);
const char *uc_launcher_state(void);
/* Read-only startup check. Pending install needs five continuous ready/idle seconds.
 * Cancel on suspend; never call startup again on resume. Manual operations cancel it. */
void uc_launcher_startup(unsigned short port);
int uc_launcher_pending(void);
void uc_launcher_poll(uint64_t now, int ready, int idle);
void uc_launcher_cancel(void);
#endif
