#ifndef UC_USB_PROTOCOL_H
#define UC_USB_PROTOCOL_H
#include "transport.h"
#include "../protocols/profiles.h"
#include <stddef.h>
/* Caller-owned, zero-copy protocol state; no sleeps or I/O in feed. */
typedef struct {
    uc_usb_kind kind;
    int step, attempts, waiting, ready, failed;
    uint64_t started, sent_at;
    uint8_t pending[64], expected_ack;
    unsigned pending_length;
    int xinput_known; /* Manager validates the XInput interface and native activation allowlist. */
    int led_sent;
    unsigned rejected_reports; /* Three malformed input packets fail the device. */
    union {
        max_align_t align;
        uint8_t bytes[PROFILE_CTX_MAX];
    } context;
} uc_usb_protocol;
void uc_usb_protocol_begin(uc_usb_protocol *, uc_usb_kind, uint64_t now);
/* Advances at most one initialization command per call; 0 progress/ready, -1 failed. */
int uc_usb_protocol_poll(uc_usb_protocol *, const uc_usb_transport *, uint64_t now);
/* Returns 1 for a valid normalized input, 0 ACK/ignored, -1 fatal. */
int uc_usb_protocol_feed(uc_usb_protocol *, const uint8_t *, size_t, pad_state *, uint64_t now);
#endif
