#include "bt_autostart.h"
#include <string.h>
void uc_bt_auto_arm(uc_bt_autostart *s, uint64_t now)
{
    memset(s, 0, sizeof *s);
    s->armed = 1;
    s->until = now + 120000;
    s->last = now;
}
void uc_bt_auto_cancel(uc_bt_autostart *s)
{
    memset(s, 0, sizeof *s);
}
void uc_bt_auto_line(uc_bt_autostart *s, const char *line)
{
    if (!s->armed || !s->fresh || !strstr(line, "[SceShellUI]") ||
        !strstr(line, "OnFocusActiveSceneChanged"))
        return;
    const char *target = strstr(line, " -> ");
    if (!target)
        return;
    s->home = strncmp(target, " -> [Render.NPXS40002 : AppScene]", 31) == 0;
    if (!s->home)
        s->timing = 0;
}
int uc_bt_auto_due(uc_bt_autostart *s, uint64_t now, int ready)
{
    if (!s->armed)
        return 0;
    if (now < s->last || now >= s->until) {
        uc_bt_auto_cancel(s);
        return 0;
    }
    s->last = now;
    if (!s->fresh || !s->home || !ready) {
        s->timing = 0;
        return 0;
    }
    if (!s->timing) {
        s->since = now;
        s->timing = 1;
    }
    if (now - s->since < UC_AUTO_BT_DELAY_MS)
        return 0;
    uc_bt_auto_cancel(s);
    return 1;
}
