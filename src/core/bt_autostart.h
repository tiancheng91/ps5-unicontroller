#ifndef UC_BT_AUTOSTART_H
#define UC_BT_AUTOSTART_H
#include <stdint.h>
#define UC_AUTO_BT_DELAY_MS 15000u
#define UC_AUTO_USB_DELAY_MS 25000u
typedef struct {
    int armed, fresh, home, timing;
    uint64_t until, since, last;
} uc_bt_autostart;
/* A bounded observation window; history is ignored until the stream drains. */
void uc_bt_auto_arm(uc_bt_autostart *s, uint64_t now);
void uc_bt_auto_cancel(uc_bt_autostart *s);
void uc_bt_auto_line(uc_bt_autostart *s, const char *line);
int uc_bt_auto_due(uc_bt_autostart *s, uint64_t now, int ready);
#endif
