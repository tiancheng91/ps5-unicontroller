#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>
static int test_open(const char *, int, ...);
static int test_close(int);
static int test_fstat(int, struct stat *);
static int test_ioctl(int, unsigned long, ...);
static ssize_t test_read(int, void *, size_t);
static ssize_t test_write(int, const void *, size_t);
#define UC_PS5
#define open test_open
#define close test_close
#define fstat test_fstat
#define ioctl test_ioctl
#define read test_read
#define write test_write
#include "../../src/drivers/usb/transport.c"
#undef open
#undef close
#undef fstat
#undef ioctl
#undef read
#undef write

static uint8_t original_cfg[] = {9, 2, 32, 0, 1,    1, 0,  0x80, 50, 9, 4, 3,    0, 2,  0xff, 0x5d,
                                 1, 0, 7,  5, 0x83, 3, 32, 0,    4,  7, 5, 0x04, 3, 32, 0,    4};
static uint8_t cfg[80];
static size_t cfg_length = sizeof(original_cfg);
static uint8_t hid_report[] = {0x05, 0x01, 0x09, 0x05, 0xa1, 0x01, 0xc0};
static int descriptor_reads, descriptor_error;
static unsigned last_config_index;
static int full_descriptor_error;
static int control_error, plugtime_error, device_descriptor_error, device_info_error;
static uint32_t plugtime_generation = 1;
static ino_t inode_generation = 1;
static unsigned vendor = 0x3537, product = 0x1040;
static int detach_count, claim_count, release_count, close_count, detach_error, claim_error;
static int fail_output, read_error, write_error, short_write;
static int test_open(const char *path, int flags, ...)
{
    assert(flags & O_NONBLOCK);
    if (!strcmp(path, "/dev/usb/2.7.0")) {
        if (control_error) {
            errno = control_error;
            return -1;
        }
        return 10;
    }
    assert(claim_count);
    if (!strcmp(path, "/dev/usb/2.7.3")) {
        assert((flags & O_ACCMODE) == O_RDONLY);
        return 11;
    }
    assert(!strcmp(path, "/dev/usb/2.7.4") && (flags & O_ACCMODE) == O_WRONLY);
    if (fail_output) {
        errno = EPERM;
        return -1;
    }
    return 12;
}
static int test_close(int fd)
{
    assert(fd >= 10 && fd <= 12);
    ++close_count;
    return 0;
}
static int test_fstat(int fd, struct stat *status)
{
    assert(fd == 10);
    memset(status, 0, sizeof(*status));
    status->st_dev = 1;
    status->st_ino = inode_generation;
    return 0;
}
static int test_ioctl(int fd, unsigned long request, ...)
{
    va_list args;
    va_start(args, request);
    void *arg = va_arg(args, void *);
    va_end(args);
    switch (request) {
    case USB_GET_DEVICE_DESC: {
        if (device_descriptor_error) {
            errno = device_descriptor_error;
            return -1;
        }
        struct usb_device_descriptor *d = arg;
        d->idVendor[0] = vendor;
        d->idVendor[1] = vendor >> 8;
        d->idProduct[0] = product;
        d->idProduct[1] = product >> 8;
        return 0;
    }
    case USB_GET_DEVICEINFO: {
        if (device_info_error) {
            errno = device_info_error;
            return -1;
        }
        struct usb_device_info *info = arg;
        info->udi_vendorNo = vendor;
        info->udi_productNo = product;
        return 0;
    }
    case USB_GET_FULL_DESC: {
        if (full_descriptor_error) {
            errno = full_descriptor_error;
            return -1;
        }
        struct usb_gen_descriptor *d = arg;
        last_config_index = d->ugd_config_index;
        memcpy(d->ugd_data, cfg, cfg_length);
        d->ugd_actlen = cfg_length;
        return 0;
    }
    case USB_GET_PLUGTIME:
        if (plugtime_error) {
            errno = plugtime_error;
            return -1;
        }
        *(uint32_t *)arg = plugtime_generation;
        return 0;
    case USB_DO_REQUEST: {
        struct usb_ctl_request *r = arg;
        ++descriptor_reads;
        assert(r->ucr_request.bmRequestType == 0x81 && r->ucr_request.bRequest == 6);
        assert(UGETW(r->ucr_request.wValue) == 0x2200 && UGETW(r->ucr_request.wIndex) == 3);
        if (descriptor_error) {
            errno = descriptor_error;
            return -1;
        }
        assert(UGETW(r->ucr_request.wLength) == sizeof(hid_report));
        memcpy(r->ucr_data, hid_report, sizeof(hid_report));
        r->ucr_actlen = sizeof(hid_report);
        return 0;
    }
    case USB_IFACE_DRIVER_DETACH:
        assert(*(int *)arg == 0); /* Descriptor index, not bInterfaceNumber=3. */
        ++detach_count;
        errno = ENOTTY;
        return detach_error ? -1 : 0;
    case USB_CLAIM_INTERFACE:
        assert(detach_count && *(int *)arg == 0);
        ++claim_count;
        errno = claim_error;
        return claim_error ? -1 : 0;
    case USB_RELEASE_INTERFACE:
        ++release_count;
        return 0;
    case USB_SET_RX_SHORT_XFER:
        assert(fd == 11 && *(int *)arg == 1);
        return 0;
    default:
        assert(0); /* No reset, configuration write or blind endpoint probes. */
        return -1;
    }
}
static ssize_t test_read(int fd, void *buffer, size_t length)
{
    assert(fd == 11 && length == 32);
    if (read_error) {
        errno = read_error;
        return -1;
    }
    memset(buffer, 0, 20);
    ((uint8_t *)buffer)[1] = 20;
    return 20;
}
static ssize_t test_write(int fd, const void *buffer, size_t length)
{
    assert(fd == 12 && buffer && length <= 32);
    if (write_error) {
        errno = write_error;
        return -1;
    }
    return short_write ? (ssize_t)length - 1 : (ssize_t)length;
}
static void reset(void)
{
    detach_count = claim_count = release_count = close_count = 0;
    detach_error = claim_error = fail_output = read_error = write_error = short_write = 0;
    vendor = 0x3537;
    product = 0x1040;
    memcpy(cfg, original_cfg, sizeof(original_cfg));
    cfg_length = sizeof(original_cfg);
    descriptor_reads = descriptor_error = control_error = plugtime_error = 0;
    plugtime_generation = 1;
    inode_generation = 1;
}
int main(void)
{
    uc_usb_transport t;
    uc_usb_device d;
    uint8_t buffer[64];
    reset();
    assert(uc_usb_transport_open(&t));
    native_transport *n = t.context;
    assert(open_candidate(n, 2, 7, &d) == 1 && detach_count == 1 && claim_count == 1);
    assert(native_read(n, buffer, sizeof(buffer)) == 20);
    read_error = EAGAIN;
    assert(!native_read(n, buffer, sizeof(buffer)));
    write_error = EAGAIN;
    assert(!native_write(n, buffer, 3));
    write_error = 0;
    assert(native_write(n, buffer, 3) == 3);
    short_write = 1;
    assert(native_write(n, buffer, 3) < 0 && native_alive(n) == 1);
    read_error = ENXIO;
    assert(native_read(n, buffer, sizeof(buffer)) < 0);
    uc_usb_transport_release(&t);
    assert(close_count == 3 && release_count == 1);
    reset();
    assert(uc_usb_transport_open(&t));
    n = t.context;
    device_descriptor_error = ENOTTY;
    assert(open_candidate(n, 2, 7, &d) == 2 && !strcmp(d.reason, "device_descriptor"));
    assert(d.diagnostic.descriptor_attempted && !d.diagnostic.descriptor_ok &&
           d.diagnostic.descriptor_errno == ENOTTY && !d.diagnostic.info_attempted);
    device_descriptor_error = 0;
    device_info_error = ENOTTY;
    assert(open_candidate(n, 2, 7, &d) == 1 && n->info_unsupported);
    assert(last_config_index == 0xff && d.diagnostic.config_index == 0xff);
    assert(d.vid == 0x3537 && d.pid == 0x1040 && d.diagnostic.descriptor_ok &&
           d.diagnostic.info_attempted && !d.diagnostic.info_ok &&
           d.diagnostic.info_errno == ENOTTY);
    assert(native_alive(n) == 1);
    vendor = 0x1234;
    assert(native_alive(n) == 0); /* Same node changed identity. */
    vendor = 0x3537;
    device_descriptor_error = ENXIO;
    assert(native_alive(n) == 0); /* Removal still releases the virtual pad. */
    device_descriptor_error = EIO;
    assert(native_alive(n) == -1); /* Do not disguise an actual I/O failure. */
    device_descriptor_error = 0;
    native_close(n);
    detach_count = claim_count = 0;
    full_descriptor_error = EIO;
    assert(open_candidate(n, 2, 7, &d) == 2 && !strcmp(d.reason, "configuration_descriptor"));
    assert(d.error_number == EIO && d.vid == 0x3537 && d.diagnostic.info_errno == ENOTTY);
    assert(!detach_count && !claim_count);
    full_descriptor_error = 0;
    device_info_error = EIO;
    assert(open_candidate(n, 2, 7, &d) == 2 && !strcmp(d.reason, "device_info"));
    assert(!detach_count && !claim_count);
    device_info_error = 0;
    vendor = 0x054c;
    assert(open_candidate(n, 2, 7, &d) == 2 && !detach_count && !claim_count &&
           !strcmp(d.reason, "system_device_excluded"));
    vendor = 0x3537;
    cfg[14] = 3; /* HID/DInput does not authorize driver detachment. */
    assert(open_candidate(n, 2, 7, &d) == 2 && !detach_count && !claim_count);
    assert(!strcmp(d.reason, "not_supported_xinput"));
    cfg[14] = 0xff;
    detach_error = 1;
    assert(open_candidate(n, 2, 7, &d) == 1 && claim_count == 1);
    native_close(n);
    claim_error = EBUSY;
    assert(open_candidate(n, 2, 7, &d) == 2 && !strcmp(d.reason, "claim_interface") &&
           d.error_number == EBUSY && n->fd == -1);
    claim_error = ENOTTY;
    int before_release = release_count;
    assert(open_candidate(n, 2, 7, &d) == 1 && !n->claimed);
    native_close(n);
    assert(release_count == before_release); /* No fictitious claim to release. */
    claim_error = 0;
    fail_output = 1;
    int released = release_count;
    assert(open_candidate(n, 2, 7, &d) == 1 && n->in_fd >= 0 && n->out_fd == -1);
    assert(d.diagnostic.output_errno == EPERM && !d.diagnostic.output_available);
    assert(native_alive(n) == 1);
    native_close(n);
    assert(release_count == released + 1);
    uc_usb_transport_release(&t);
    reset();
    assert(uc_usb_transport_open(&t));
    n = t.context;
    const uint8_t hid_cfg[] = {
        9, 2, 34, 0,    1, 1,  0,    0x80, 50, 9, 4, 3,    0,
        1, 3, 0,  0,    0, 9,  0x21, 0x11, 1,  0, 1, 0x22, sizeof(hid_report),
        0, 7, 5,  0x83, 3, 32, 0,    4};
    memcpy(cfg, hid_cfg, sizeof(hid_cfg));
    cfg_length = sizeof(hid_cfg);
    cfg[16] = 1; /* Not a non-boot gamepad protocol: diagnostics only. */
    assert(open_candidate(n, 2, 7, &d) == 2 && d.kind == UC_USB_GENERIC_HID);
    assert(!strcmp(d.reason, "generic_hid_interface_unsupported") && d.diagnostic.gamepad);
    assert(!detach_count && !claim_count && descriptor_reads == 1);
    uc_usb_transport other;
    assert(uc_usb_transport_open(&other));
    assert(open_candidate(other.context, 2, 7, &d) == 2 && descriptor_reads == 1);
    uc_usb_transport_release(&other);
    char removed[24];
    assert(!uc_usb_transport_take_removed(removed, sizeof(removed)) && hid_cache_count == 1);
    control_error = EACCES;
    assert(!uc_usb_transport_take_removed(removed, sizeof(removed)) && hid_cache_count == 1);
    control_error = ENOENT;
    assert(uc_usb_transport_take_removed(removed, sizeof(removed)) && !hid_cache_count);
    assert(!strcmp(removed, "/dev/ugen2.7"));
    control_error = 0;
    assert(open_candidate(n, 2, 7, &d) == 2 && descriptor_reads == 2);
    ++plugtime_generation; /* Fast replacement, same pathname and inode. */
    assert(uc_usb_transport_take_removed(removed, sizeof(removed)) && !hid_cache_count);
    assert(open_candidate(n, 2, 7, &d) == 2 && descriptor_reads == 3);
    ++plugtime_generation; /* Re-enumeration is also checked before cache reuse. */
    assert(open_candidate(n, 2, 7, &d) == 2 && descriptor_reads == 4);
    plugtime_error = ENOTTY;
    ++inode_generation; /* Firmware without plugtime still has node identity fallback. */
    assert(uc_usb_transport_take_removed(removed, sizeof(removed)) && !hid_cache_count);
    descriptor_error = EIO;
    assert(open_candidate(n, 2, 7, &d) == 2 && d.diagnostic.report_errno == EIO);
    int reads_before_retry = descriptor_reads;
    assert(open_candidate(n, 2, 7, &d) == 2 && descriptor_reads == reads_before_retry);
    control_error = ENXIO;
    assert(uc_usb_transport_take_removed(removed, sizeof(removed)) && !hid_cache_count);
    control_error = descriptor_error = 0;
    assert(open_candidate(n, 2, 7, &d) == 2 && d.diagnostic.report_valid &&
           descriptor_reads == reads_before_retry + 1);
    assert(!detach_count && !claim_count);
    cfg[16] = 0; /* Verified non-boot gamepad: only its input endpoint is opened. */
    assert(open_candidate(n, 2, 7, &d) == 1 && d.kind == UC_USB_GENERIC_HID);
    assert(n->in_fd == 11 && n->out_fd == -1 && !d.ep_out);
    assert(detach_count == 1 && claim_count == 1);
    uc_usb_transport_release(&t);
    puts(
        "USB native transport: selection, detach, claim, endpoints, backpressure and cleanup PASS");
    return 0;
}
