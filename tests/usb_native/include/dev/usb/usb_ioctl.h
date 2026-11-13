#ifndef UC_TEST_USB_IOCTL_H
#define UC_TEST_USB_IOCTL_H
/* Unique mock request IDs. Actual ioctl ABI is checked by the PS5 SDK build. */
#define USB_GET_DEVICE_DESC 1
#define USB_GET_DEVICEINFO 2
#define USB_GET_FULL_DESC 3
#define USB_IFACE_DRIVER_DETACH 4
#define USB_CLAIM_INTERFACE 5
#define USB_RELEASE_INTERFACE 6
#define USB_SET_RX_SHORT_XFER 7
#define USB_DO_REQUEST 8
#define USB_GET_PLUGTIME 9
struct usb_device_info {
    unsigned udi_config_index;
    unsigned udi_vendorNo, udi_productNo;
};
struct usb_gen_descriptor {
    void *ugd_data;
    unsigned ugd_maxlen, ugd_actlen, ugd_config_index;
};
struct usb_ctl_request {
    void *ucr_data;
    unsigned ucr_actlen;
    struct usb_device_request ucr_request;
};
#endif
