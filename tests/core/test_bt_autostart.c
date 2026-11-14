#include "../../src/core/bt_autostart.h"
#include <assert.h>
#include <stdio.h>
static const char *home =
    "[SceShellUI] OnFocusActiveSceneChanged [Login] -> [Render.NPXS40002 : AppScene]";
static const char *away = "[SceShellUI] OnFocusActiveSceneChanged [Home] -> [Login : AppScene]";
int main(void)
{
    uc_bt_autostart s = {0};
    uc_bt_auto_line(&s, home);
    assert(!uc_bt_auto_due(&s, 9999, 1));
    uc_bt_auto_arm(&s, 10000);
    uc_bt_auto_line(&s, home); /* Buffered history never starts a countdown. */
    assert(!s.home);
    s.fresh = 1;
    assert(!uc_bt_auto_due(&s, 15000, 1)); /* Users alone cannot trigger. */
    uc_bt_auto_line(&s, home);
    assert(!uc_bt_auto_due(&s, 16000, 0));
    assert(!uc_bt_auto_due(&s, 20000, 1));
    assert(!uc_bt_auto_due(&s, 24999, 1));
    uc_bt_auto_line(&s, away);
    assert(!uc_bt_auto_due(&s, 25000, 1));
    uc_bt_auto_line(&s, home);
    assert(!uc_bt_auto_due(&s, 26000, 1));
    assert(!uc_bt_auto_due(&s, 28000, 0)); /* Rest/user loss/cleanup blocks. */
    assert(!uc_bt_auto_due(&s, 30000, 1));
    assert(!uc_bt_auto_due(&s, 34999, 1));
    assert(!uc_bt_auto_due(&s, 35000, 1));
    assert(!uc_bt_auto_due(&s, 44999, 1));
    assert(uc_bt_auto_due(&s, 45000, 1));
    uc_bt_auto_line(&s, home);
    assert(!uc_bt_auto_due(&s, 46000, 1)); /* One attempt, no failure retry. */
    uc_bt_auto_arm(&s, 50000);             /* Resume resets freshness and focus. */
    assert(!s.home && !s.fresh);
    assert(!uc_bt_auto_due(&s, 56000, 1));
    s.fresh = 1;
    uc_bt_auto_line(&s, home);
    assert(!uc_bt_auto_due(&s, 57000, 1));
    uc_bt_auto_cancel(&s);
    assert(!uc_bt_auto_due(&s, 65000, 1));
    uc_bt_auto_arm(&s, 70000);
    s.fresh = 1;
    uc_bt_auto_line(&s, home);
    assert(!uc_bt_auto_due(&s, 190000, 1) && !s.armed); /* Window expired. */
    uc_bt_auto_arm(&s, 200000);
    assert(!uc_bt_auto_due(&s, 1, 1) && !s.armed); /* Clock regression fails closed. */
    puts("Bluetooth auto-start: fresh focus, delay, suspend reset, cancellation and timeout PASS");
}
