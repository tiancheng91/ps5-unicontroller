#ifndef UC_USB_H
#define UC_USB_H
#include "usb/transport.h"
/* Single event-loop, same return convention as Bluetooth manager. No auto-start. */
int uc_usb_start(int32_t user);
int uc_usb_stop(void);
void uc_usb_poll(uint64_t now);
void uc_usb_suspend(void);
int uc_usb_running(void);
void uc_usb_set_log_level(unsigned level);
void uc_usb_set_rumble_enabled(int enabled);
int uc_usb_assign(unsigned slot, const char *node, int32_t user);
size_t uc_usb_status_json(char *buffer, size_t capacity);
/* Full bounded descriptor snapshots; no hardware I/O when exported. */
size_t uc_usb_diagnostics_json(char *buffer, size_t capacity);
/* Test injection only while fully stopped; copied transport table/context borrowed. */
int uc_usb_set_transport(const uc_usb_transport *transport);
int uc_usb_set_transports(const uc_usb_transport *transports, size_t count);
int uc_usb_device_options_match(const char *key, uint64_t session);
void uc_usb_device_options_changed(void);
/* 500ms pulse: 0 accepted, 2 rate-limited, negative unavailable. Identity must match. */
int uc_usb_test_rumble(const char *key, uint64_t session, uint64_t now);
#endif
