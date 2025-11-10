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
 * Detection strategy:
 * 1. Check if LIBUSB_API_VERSION indicates modern libusb (1.0.27+) with the function
 * 2. Check if __FreeBSD__ is defined (FreeBSD base libusb always has it)
 * 3. Otherwise provide fallback
 *
 * Note: We can't reliably use CMake's HAVE_LIBUSB_GET_STRING_DESCRIPTOR because
 * the detection may fail even when the function exists.
 */

/* Determine if we need the fallback */
#undef NEED_LIBUSB_GET_STRING_DESCRIPTOR_FALLBACK

#if defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
  /* BSD systems have libusb_get_string_descriptor in base system */
  /* Do NOT provide fallback */
#elif defined(LIBUSB_API_VERSION) && LIBUSB_API_VERSION >= 0x01000109
  /* libusb >= 1.0.27 has the function */
  /* Do NOT provide fallback */
#else
  /* Old libusb or unknown version - provide fallback */
  #define NEED_LIBUSB_GET_STRING_DESCRIPTOR_FALLBACK 1
#endif

#ifdef NEED_LIBUSB_GET_STRING_DESCRIPTOR_FALLBACK
/*
 * Fallback implementation for older libusb versions that lack
 * libusb_get_string_descriptor (added in libusb 1.0.27, 2024)
 */
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
