# cmake/DetectLibusbStringDescriptor.cmake
include_guard(GLOBAL)
include(CheckSymbolExists)  # <-- the real built-in module

# Probe for libusb_get_string_descriptor in the visible headers
# On FreeBSD base, it's in <libusb.h>; on Linux/macOS pkg libusb it's in <libusb-1.0/libusb.h>
# Try the common case first. If you want to be extra robust, you can add a second probe later.
check_symbol_exists(libusb_get_string_descriptor "libusb.h" HAVE_LIBUSB_GET_STRING_DESCRIPTOR)

# Export a preprocessor define; or, if you prefer, write it into your generated config header.
add_compile_definitions(HAVE_LIBUSB_GET_STRING_DESCRIPTOR=$<BOOL:${HAVE_LIBUSB_GET_STRING_DESCRIPTOR}>)

