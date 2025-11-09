include_guard(GLOBAL)

# Global C flags
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Feature toggles
option(ENABLE_AVAHI        "Enable Avahi (service discovery)" ON)
option(ENABLE_SDR_DRIVERS  "Build SDR driver modules"        ON)
option(ENABLE_XATTR        "Build xattr helpers (attr.c)"    ON)

# FreeBSD quality-of-life: pkg-config & rpath defaults
if(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
  list(APPEND CMAKE_PREFIX_PATH /usr/local)
  foreach(_pcdir IN ITEMS /usr/local/libdata/pkgconfig /usr/local/lib/pkgconfig)
    if(EXISTS "${_pcdir}")
      if(DEFINED ENV{PKG_CONFIG_PATH})
        set(ENV{PKG_CONFIG_PATH} "$ENV{PKG_CONFIG_PATH}:${_pcdir}")
      else()
        set(ENV{PKG_CONFIG_PATH} "${_pcdir}")
      endif()
    endif()
  endforeach()
  set(CMAKE_BUILD_RPATH  "/usr/local/lib")
  set(CMAKE_INSTALL_RPATH "/usr/local/lib")
  set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)
endif()

# Find pthreads everywhere
find_package(Threads REQUIRED)

