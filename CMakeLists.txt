cmake_minimum_required(VERSION 3.16)
project(ka9q-radio VERSION 0.1 LANGUAGES C)

# Set default build type if not specified
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build (Debug, Release, RelWithDebInfo, MinSizeRel)" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "RelWithDebInfo" "MinSizeRel")
endif()

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

# Make our project modules discoverable
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# Load configuration modules
include(Options)                          # Feature flags, standards, FreeBSD helpers
include(CompilerFlags)                    # Compiler options via interface target
include(DetectSockets)                    # Platform header checks
include(DetectAlloca)                     # alloca availability
include(FindDeps)                         # Dependencies via pkg-config
include(DetectLibusbStringDescriptor)     # libusb feature probe

# Enable compile_commands.json for IDE/language server support
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build subdirectories
add_subdirectory(src)
add_subdirectory(service)
add_subdirectory(rules)
add_subdirectory(aux)
add_subdirectory(share)

# Installation of data files (you may want to expand this)
# install(DIRECTORY share/ DESTINATION share/ka9q-radio)
# install(DIRECTORY config/ DESTINATION etc/radio)

# Print configuration summary
message(STATUS "")
message(STATUS "Configuration Summary:")
message(STATUS "  Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  Install prefix: ${CMAKE_INSTALL_PREFIX}")
message(STATUS "  Native arch: ${ENABLE_NATIVE_ARCH}")
message(STATUS "  SDR drivers: ${ENABLE_SDR_DRIVERS}")
message(STATUS "  Avahi: ${ENABLE_AVAHI}")
message(STATUS "  xattr: ${ENABLE_XATTR}")
message(STATUS "")
