find_path(HIDAPI_INCLUDE_DIRS hidapi/hidapi.h)
find_library(HIDAPI_LIBRARIES NAMES hidapi-hidraw hidapi-libusb hidapi)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(HIDAPI DEFAULT_MSG HIDAPI_LIBRARIES HIDAPI_INCLUDE_DIRS)

mark_as_advanced(HIDAPI_INCLUDE_DIRS HIDAPI_LIBRARIES)

