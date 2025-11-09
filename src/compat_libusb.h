#ifndef COMPAT_LIBUSB_H
#define COMPAT_LIBUSB_H

/* Normalize header location across platforms */
#if __has_include(<libusb.h>)
#  include <libusb.h>                     /* FreeBSD base, OpenBSD */
#elif __has_include(<libusb-1.0/libusb.h>)
#  include <libusb-1.0/libusb.h>          /* Linux, macOS/Homebrew, many distros */
#else
#  error "No usable libusb header found. Install libusb-1.0 development headers."
#endif

/*
 * Optional compatibility: only provide the fallback when the system lib
 * lacks libusb_get_string_descriptor(). CMake defines the macro below.
 */
#ifndef HAVE_LIBUSB_GET_STRING_DESCRIPTOR
static inline int
libusb_get_string_descriptor(libusb_device_handle *dev,
                             uint8_t desc_index, uint16_t langid,
                             unsigned char *data, int length)
{
    return libusb_control_transfer(dev,
        LIBUSB_ENDPOINT_IN,
        LIBUSB_REQUEST_GET_DESCRIPTOR,
        (LIBUSB_DT_STRING << 8) | desc_index,
        langid,
        data, (uint16_t)length,
        1000);
}
#endif

#endif /* COMPAT_LIBUSB_H */
