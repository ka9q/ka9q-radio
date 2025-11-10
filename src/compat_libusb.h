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
 * Compatibility fallback for libusb_get_string_descriptor
 * 
 * This function was added in libusb 1.0.27 (2024). Older versions need a fallback.
 * We detect this by checking LIBUSB_API_VERSION.
 */

#if defined(LIBUSB_API_VERSION)
  /* LIBUSB_API_VERSION format: 0x01000109 means 1.0.27 */
  /* The function was added in version 1.0.27 (API version 0x01000109) */
  #if LIBUSB_API_VERSION < 0x01000109
    /* Old libusb - needs fallback */
    #define NEED_LIBUSB_GET_STRING_DESCRIPTOR_FALLBACK 1
  #endif
#else
  /* Very old libusb without LIBUSB_API_VERSION - definitely needs fallback */
  #define NEED_LIBUSB_GET_STRING_DESCRIPTOR_FALLBACK 1
#endif

#ifdef NEED_LIBUSB_GET_STRING_DESCRIPTOR_FALLBACK
/* Provide fallback implementation for older libusb versions */
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
#endif /* NEED_LIBUSB_GET_STRING_DESCRIPTOR_FALLBACK */

#endif /* COMPAT_LIBUSB_H */
