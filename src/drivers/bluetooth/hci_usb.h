/* HCI over USB on the PS5 Bluetooth chip, shared with system software.
 * Descriptor selection identifies an HCI function, not exclusive ownership.
 * Firmware observations do NOT prove which function the system uses. We do not
 * detach/reset the chip; queued reads and commands can still contend with it.
 * See hci_usb.c for the unchanged existing queue/priority settings.
 */
#ifndef PH_HCI_USB_H
#define PH_HCI_USB_H

#include "hci.h"

/* Opens the controller. Returns 0 on failure, with the reason logged. */
int hci_usb_open(hci_t *out);

#endif
