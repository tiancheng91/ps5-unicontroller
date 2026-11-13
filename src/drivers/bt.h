#ifndef UC_BT_H
#define UC_BT_H
#include <stddef.h>
#include <stdint.h>
#include "bluetooth/host.h"
/* Single event-loop owner. Explicit start only; no transport auto-reopen.
 * 0 success, 2 cleanup pending/busy, -1 unsupported/error. */
int uc_bt_start(int32_t user);
/* Experimental resume: one physical source drives an exclusive, initially
 * unbound virtual pad. PS5 user selection owns binding; never assume success. */
int uc_bt_start_system_assigned(void);
/* Assign a connected slot only if its current canonical address still matches.
 * New assignments require a fresh report; additional pads never auto-inherit.
 * 2 means accepted reassignment awaiting owned cleanup; conflict returns -1. */
int uc_bt_assign(int slot, const char *address, int32_t user);
int uc_bt_stop(void);
void uc_bt_poll(uint64_t now_ms);
int uc_bt_scan(unsigned seconds); /* 0 stops discovery, preserving connections. */
int uc_bt_pair(uint32_t discovery_id);
int uc_bt_forget(const char *address);
size_t uc_bt_status_json(char *buffer, size_t capacity);
size_t uc_bt_diagnostics_json(char *buffer, size_t capacity);
int uc_bt_running(void);
void uc_bt_suspend(void);
void uc_bt_set_log_level(unsigned level);
/* Global rumble setting also controls the delayed connection notification. */
void uc_bt_set_rumble_enabled(int enabled);
/* Injection is only accepted while fully stopped. The copied database path
 * must be a trusted writable location; the default is /data/unicontroller/bonds.db.
 * Factory owns HCI cleanup on open failure, host_close owns it on success. */
typedef struct {
    host_t *(*open)(const char *database_path, const host_events *events, void *userdata);
    void *userdata;
} uc_bt_factory;
int uc_bt_set_factory(const uc_bt_factory *factory, const char *database_path);
int uc_bt_device_options_match(const char *key, uint64_t session);
void uc_bt_device_options_changed(void);
#endif
