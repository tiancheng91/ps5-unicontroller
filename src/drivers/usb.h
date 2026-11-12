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
size_t uc_usb_status_json(char *buffer, size_t capacity);
/* Test injection only while fully stopped; copied transport table/context borrowed. */
int uc_usb_set_transport(const uc_usb_transport *transport);
#endif
