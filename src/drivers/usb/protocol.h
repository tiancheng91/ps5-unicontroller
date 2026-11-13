#ifndef UC_USB_PROTOCOL_H
#define UC_USB_PROTOCOL_H
#include "transport.h"
#include "generic_hid.h"
#include "../protocols/profiles.h"
#include <stddef.h>
/* Caller-owned, zero-copy protocol state; no sleeps or I/O in feed. */
typedef struct {
    uc_usb_kind kind;
    uc_usb_generic_hid hid;
    int ready, failed;
    uint64_t started;
    int led_sent, led_failed;
    unsigned led_attempts;
    uint64_t led_last_at;
    unsigned rejected_reports; /* Three malformed input packets fail the device. */
    uint8_t rumble_strong, rumble_weak, rumble_packet[64];
    unsigned rumble_length, rumble_attempts, rumble_stops;
    int rumble_seen, rumble_sample, rumble_dirty, rumble_sent, rumble_failed;
    uint64_t rumble_sample_at, rumble_sent_at, rumble_retry_at, rumble_queued_at;
} uc_usb_protocol;
int uc_usb_protocol_hid_descriptor(uc_usb_protocol *, const uc_usb_device *);
void uc_usb_protocol_begin(uc_usb_protocol *, uc_usb_kind, uint64_t now);
/* Advances at most one initialization command per call; 0 progress/ready, -1 failed. */
int uc_usb_protocol_poll(uc_usb_protocol *, const uc_usb_transport *, uint64_t now);
/* Queue a fresh motor sample, without I/O. 0 accepted, -1 unready/failed.
 * poll is the sole writer: initialization/LED precede output, changed samples
 * supersede unsent packets, and nonzero samples expire after 500ms. */
int uc_usb_protocol_rumble(uc_usb_protocol *, uint8_t strong, uint8_t weak, uint64_t now);
/* Returns 1 for a valid normalized input, 0 ACK/ignored, -1 fatal. */
int uc_usb_protocol_feed(uc_usb_protocol *, const uint8_t *, size_t, pad_state *, uint64_t now);
#endif
