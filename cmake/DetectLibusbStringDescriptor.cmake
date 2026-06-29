# cmake/DetectLibusbStringDescriptor.cmake
# Probe for libusb_get_string_descriptor availability
# This function exists in newer libusb versions but may be missing in older ones
# The detection is actually done in FindDeps.cmake after libusb is located

include_guard(GLOBAL)

# Note: The actual detection happens in FindDeps.cmake because we need to know
# where libusb headers are located first. This module is kept for compatibility
# but doesn't perform the actual check anymore.

# The check in FindDeps.cmake uses:
# - Proper include paths from LibUSB::libusb-1.0 target
# - check_symbol_exists() with compat_libusb.h
# - Results cached in HAVE_LIBUSB_GET_STRING_DESCRIPTOR

message(STATUS "libusb string descriptor detection delegated to FindDeps.cmake")
