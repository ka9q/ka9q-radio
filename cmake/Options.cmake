# cmake/Options.cmake
# Global build options and platform-specific configuration
include_guard(GLOBAL)

# ==================== C STANDARD ====================
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)  # Enable GNU extensions (gnu11)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ==================== FEATURE OPTIONS ====================
option(ENABLE_AVAHI        "Enable Avahi service discovery" ON)
option(ENABLE_SDR_DRIVERS  "Build SDR driver modules (airspy, rtlsdr, hackrf, etc.)" ON)
option(ENABLE_XATTR        "Build extended attribute helpers (attr.c)" ON)

# ==================== FREEBSD CONFIGURATION ====================
# FreeBSD needs special handling for pkg-config and library paths
if(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
  message(STATUS "Configuring for FreeBSD")
  
  # Add /usr/local to search paths (where ports/pkg install)
  list(APPEND CMAKE_PREFIX_PATH /usr/local)
  
  # Setup pkg-config search paths for FreeBSD
  foreach(_pcdir IN ITEMS /usr/local/libdata/pkgconfig /usr/local/lib/pkgconfig)
    if(EXISTS "${_pcdir}")
      if(DEFINED ENV{PKG_CONFIG_PATH})
        set(ENV{PKG_CONFIG_PATH} "$ENV{PKG_CONFIG_PATH}:${_pcdir}")
      else()
        set(ENV{PKG_CONFIG_PATH} "${_pcdir}")
      endif()
      message(STATUS "Added to PKG_CONFIG_PATH: ${_pcdir}")
    endif()
  endforeach()
  
  # RPATH settings for FreeBSD (helps find libs at runtime)
  set(CMAKE_BUILD_RPATH  "/usr/local/lib")
  set(CMAKE_INSTALL_RPATH "/usr/local/lib")
  set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)
endif()

# ==================== THREADING ====================
# Find pthreads - required on all platforms
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

if(NOT Threads_FOUND)
  message(FATAL_ERROR "POSIX threads not found - required for ka9q-radio")
endif()

message(STATUS "Threading library: ${CMAKE_THREAD_LIBS_INIT}")
