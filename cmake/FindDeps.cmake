# cmake/FindDeps.cmake
# Dependency discovery and imported target creation
include_guard(GLOBAL)

find_package(PkgConfig REQUIRED)
include(CheckSymbolExists)
include(CheckCSourceCompiles)
include(CheckIncludeFile)

# ==================== HELPER: Clean stale paths from pkg-config targets ====================
# Homebrew sometimes updates packages leaving stale paths in .pc files
function(kr_sanitize_pkgconfig_target target_name)
  if(NOT TARGET ${target_name})
    return()
  endif()
  
  # Get the include directories
  get_target_property(_includes ${target_name} INTERFACE_INCLUDE_DIRECTORIES)
  if(_includes)
    set(_clean_includes)
    foreach(_inc IN LISTS _includes)
      if(EXISTS "${_inc}")
        list(APPEND _clean_includes "${_inc}")
      else()
        message(STATUS "  Removing non-existent include path: ${_inc}")
      endif()
    endforeach()
    if(_clean_includes)
      set_target_properties(${target_name} PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${_clean_includes}")
    else()
      set_property(TARGET ${target_name} PROPERTY INTERFACE_INCLUDE_DIRECTORIES "")
    endif()
  endif()
endfunction()

# ==================== HELPER: Validate that a header can actually be found ====================
# Some pkg-config files have incorrect include paths
function(kr_validate_header target_name header_file)
  if(NOT TARGET ${target_name})
    set(${target_name}_HEADER_VALID FALSE PARENT_SCOPE)
    return()
  endif()
  
  # Get include directories from the target
  get_target_property(_includes ${target_name} INTERFACE_INCLUDE_DIRECTORIES)
  
  if(NOT _includes)
    set(${target_name}_HEADER_VALID FALSE PARENT_SCOPE)
    return()
  endif()
  
  # Try to find the header in the include paths
  set(_found FALSE)
  foreach(_inc IN LISTS _includes)
    if(EXISTS "${_inc}/${header_file}")
      set(_found TRUE)
      break()
    endif()
  endforeach()
  
  if(_found)
    set(${target_name}_HEADER_VALID TRUE PARENT_SCOPE)
  else()
    # Maybe the header is one level up (common pkg-config issue)
    foreach(_inc IN LISTS _includes)
      get_filename_component(_parent "${_inc}" DIRECTORY)
      if(EXISTS "${_parent}/${header_file}")
        message(STATUS "  Adjusting include path for ${header_file}: ${_parent}")
        set_target_properties(${target_name} PROPERTIES
          INTERFACE_INCLUDE_DIRECTORIES "${_parent}")
        set(${target_name}_HEADER_VALID TRUE PARENT_SCOPE)
        return()
      endif()
    endforeach()
    
    message(STATUS "  Cannot find ${header_file} in include paths for ${target_name}")
    set(${target_name}_HEADER_VALID FALSE PARENT_SCOPE)
  endif()
endfunction()

# ==================== FFTW (FLOAT) ====================
pkg_check_modules(FFTW3F IMPORTED_TARGET QUIET fftw3f)
if(TARGET PkgConfig::FFTW3F)
  kr_sanitize_pkgconfig_target(PkgConfig::FFTW3F)
  add_library(FFTW::fftw3f ALIAS PkgConfig::FFTW3F)
  set(HAVE_FFTW3F TRUE)
  message(STATUS "Found FFTW3f: ${FFTW3F_LIBRARIES}")
else()
  message(STATUS "FFTW3f not found - some features may be unavailable")
endif()

# FFTW threads sublib
pkg_check_modules(FFTW3F_THR IMPORTED_TARGET QUIET fftw3f_threads)
if(TARGET PkgConfig::FFTW3F_THR)
  kr_sanitize_pkgconfig_target(PkgConfig::FFTW3F_THR)
  add_library(FFTW::fftw3f_threads ALIAS PkgConfig::FFTW3F_THR)
  set(HAVE_FFTW3F_THREADS TRUE)
  message(STATUS "Found FFTW3f threads support")
else()
  find_library(FFTW3F_THREADS_LIBRARY NAMES fftw3f_threads)
  if(FFTW3F_THREADS_LIBRARY)
    add_library(FFTW::fftw3f_threads UNKNOWN IMPORTED)
    set_target_properties(FFTW::fftw3f_threads PROPERTIES
      IMPORTED_LOCATION "${FFTW3F_THREADS_LIBRARY}")
    set(HAVE_FFTW3F_THREADS TRUE)
    message(STATUS "Found FFTW3f threads (manual): ${FFTW3F_THREADS_LIBRARY}")
  else()
    message(STATUS "FFTW3f threads not found - single-threaded FFTW only")
  endif()
endif()

# ==================== OPUS ====================
pkg_check_modules(OPUS QUIET opus)

if(NOT OPUS_FOUND)
  find_path(OPUS_INCLUDE_DIRS
            NAMES opus/opus.h opus.h
            PATHS /opt/homebrew/include /usr/local/include /usr/include)
  find_library(OPUS_LIBRARIES
               NAMES opus
               PATHS /opt/homebrew/lib /usr/local/lib /usr/lib)
  if(OPUS_INCLUDE_DIRS AND OPUS_LIBRARIES)
    set(OPUS_FOUND TRUE)
  endif()
endif()

if(OPUS_FOUND AND NOT OPUS_LIBRARY_DIRS AND OPUS_LIBRARIES)
  set(_opus_dirs)
  foreach(_lib IN LISTS OPUS_LIBRARIES)
    if(IS_ABSOLUTE "${_lib}")
      get_filename_component(_dir "${_lib}" DIRECTORY)
      list(APPEND _opus_dirs "${_dir}")
    endif()
  endforeach()
  list(REMOVE_DUPLICATES _opus_dirs)
  if(_opus_dirs)
    set(OPUS_LIBRARY_DIRS "${_opus_dirs}")
  endif()
endif()

if(OPUS_FOUND AND NOT TARGET opus::opus)
  add_library(opus::opus INTERFACE IMPORTED)
  target_include_directories(opus::opus INTERFACE ${OPUS_INCLUDE_DIRS})
  if(OPUS_LIBRARY_DIRS)
    target_link_directories(opus::opus INTERFACE ${OPUS_LIBRARY_DIRS})
  endif()
  target_link_libraries(opus::opus INTERFACE ${OPUS_LIBRARIES})
  message(STATUS "Found Opus: ${OPUS_LIBRARIES}")
else()
  message(FATAL_ERROR "Opus codec library not found. Install with:\n  macOS: brew install opus\n  Debian/Ubuntu: apt install libopus-dev\n  FreeBSD: pkg install opus")
endif()

set(OPUS_FOUND ${OPUS_FOUND} CACHE INTERNAL "")
set(OPUS_INCLUDE_DIRS ${OPUS_INCLUDE_DIRS} CACHE INTERNAL "")
set(OPUS_LIBRARY_DIRS ${OPUS_LIBRARY_DIRS} CACHE INTERNAL "")
set(OPUS_LIBRARIES ${OPUS_LIBRARIES} CACHE INTERNAL "")

# ==================== LIBBSD (LINUX) ====================
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  pkg_check_modules(LIBBSD IMPORTED_TARGET QUIET libbsd)
  if(TARGET PkgConfig::LIBBSD)
    kr_sanitize_pkgconfig_target(PkgConfig::LIBBSD)
    add_library(LibBSD::libbsd ALIAS PkgConfig::LIBBSD)
    message(STATUS "Found libbsd: ${LIBBSD_LIBRARIES}")
  endif()
endif()

# ==================== INIPARSER ====================
pkg_check_modules(INIPARSER QUIET iniparser)

if(NOT INIPARSER_FOUND)
  find_path(INIPARSER_INCLUDE_DIRS
            NAMES iniparser/iniparser.h iniparser.h
            PATHS /opt/homebrew/include /usr/local/include /usr/include)
  find_library(INIPARSER_LIBRARIES
               NAMES iniparser
               PATHS /opt/homebrew/lib /usr/local/lib /usr/lib
                     /usr/lib/x86_64-linux-gnu /usr/lib/i386-linux-gnu)
  if(INIPARSER_INCLUDE_DIRS AND INIPARSER_LIBRARIES)
    set(INIPARSER_FOUND TRUE)
  endif()
endif()

if(INIPARSER_FOUND AND NOT INIPARSER_LIBRARY_DIRS AND INIPARSER_LIBRARIES)
  set(_iniparser_dirs)
  foreach(_lib IN LISTS INIPARSER_LIBRARIES)
    if(IS_ABSOLUTE "${_lib}")
      get_filename_component(_dir "${_lib}" DIRECTORY)
      list(APPEND _iniparser_dirs "${_dir}")
    endif()
  endforeach()
  list(REMOVE_DUPLICATES _iniparser_dirs)
  if(_iniparser_dirs)
    set(INIPARSER_LIBRARY_DIRS "${_iniparser_dirs}")
  endif()
endif()

if(INIPARSER_FOUND AND NOT TARGET iniparser::iniparser)
  add_library(iniparser::iniparser INTERFACE IMPORTED)
  target_include_directories(iniparser::iniparser INTERFACE ${INIPARSER_INCLUDE_DIRS})
  if(INIPARSER_LIBRARY_DIRS)
    target_link_directories(iniparser::iniparser INTERFACE ${INIPARSER_LIBRARY_DIRS})
  endif()
  target_link_libraries(iniparser::iniparser INTERFACE ${INIPARSER_LIBRARIES})
  message(STATUS "Found iniparser: ${INIPARSER_LIBRARIES}")
else()
  message(FATAL_ERROR "iniparser not found. Install with:\n  macOS: brew install iniparser\n  Debian/Ubuntu: apt install libiniparser-dev\n  FreeBSD: pkg install iniparser")
endif()

set(INIPARSER_FOUND ${INIPARSER_FOUND} CACHE INTERNAL "")
set(INIPARSER_INCLUDE_DIRS ${INIPARSER_INCLUDE_DIRS} CACHE INTERNAL "")
set(INIPARSER_LIBRARY_DIRS ${INIPARSER_LIBRARY_DIRS} CACHE INTERNAL "")
set(INIPARSER_LIBRARIES ${INIPARSER_LIBRARIES} CACHE INTERNAL "")

# ==================== HIDAPI ====================
pkg_check_modules(HIDAPI QUIET hidapi hidapi-libusb hidapi-hidraw)

if(NOT HIDAPI_FOUND)
  find_path(HIDAPI_INCLUDE_DIRS NAMES hidapi/hidapi.h
            PATHS /opt/homebrew/include /usr/local/include /usr/include)
  find_library(HIDAPI_LIBRARIES NAMES hidapi hidapi-libusb hidapi-hidraw
               PATHS /opt/homebrew/lib /usr/local/lib /usr/lib)
  if(HIDAPI_LIBRARIES)
    get_filename_component(HIDAPI_LIBRARY_DIRS "${HIDAPI_LIBRARIES}" DIRECTORY)
  endif()
  if(HIDAPI_INCLUDE_DIRS AND HIDAPI_LIBRARIES)
    set(HIDAPI_FOUND TRUE)
  endif()
endif()

set(HIDAPI_REQUIRED_FOR_RADIOD OFF)

if(APPLE)
  if(NOT HIDAPI_FOUND)
    message(FATAL_ERROR "HIDAPI required on macOS for Funcube and other HID devices.\n  Install with: brew install hidapi")
  endif()
  set(HIDAPI_REQUIRED_FOR_RADIOD ON)
elseif(UNIX AND NOT APPLE)
  if(NOT HIDAPI_FOUND)
    message(WARNING "HIDAPI not found - Funcube and HID-based SDRs will not be available.\n  Install with: sudo apt install libhidapi-dev (Debian/Ubuntu)")
  else()
    set(HIDAPI_REQUIRED_FOR_RADIOD ON)
  endif()
endif()

if(HIDAPI_FOUND AND NOT TARGET hidapi::hidapi)
  add_library(hidapi::hidapi INTERFACE IMPORTED)
  target_include_directories(hidapi::hidapi INTERFACE ${HIDAPI_INCLUDE_DIRS})
  if(HIDAPI_LIBRARY_DIRS)
    target_link_directories(hidapi::hidapi INTERFACE ${HIDAPI_LIBRARY_DIRS})
  endif()
  target_link_libraries(hidapi::hidapi INTERFACE ${HIDAPI_LIBRARIES})
  message(STATUS "Found HIDAPI: ${HIDAPI_LIBRARIES}")
endif()

set(HIDAPI_FOUND ${HIDAPI_FOUND} CACHE INTERNAL "")
set(HIDAPI_INCLUDE_DIRS ${HIDAPI_INCLUDE_DIRS} CACHE INTERNAL "")
set(HIDAPI_LIBRARIES ${HIDAPI_LIBRARIES} CACHE INTERNAL "")
set(HIDAPI_LIBRARY_DIRS ${HIDAPI_LIBRARY_DIRS} CACHE INTERNAL "")
set(HIDAPI_REQUIRED_FOR_RADIOD ${HIDAPI_REQUIRED_FOR_RADIOD} CACHE INTERNAL "")

# ==================== AUDIO/UI LIBRARIES ====================
pkg_check_modules(PORTAUDIO IMPORTED_TARGET QUIET portaudio-2.0)
if(TARGET PkgConfig::PORTAUDIO)
  kr_sanitize_pkgconfig_target(PkgConfig::PORTAUDIO)
  add_library(PortAudio::portaudio ALIAS PkgConfig::PORTAUDIO)
  message(STATUS "Found PortAudio")
endif()

if(NOT APPLE)
  pkg_check_modules(ALSA IMPORTED_TARGET QUIET alsa)
  if(TARGET PkgConfig::ALSA)
    kr_sanitize_pkgconfig_target(PkgConfig::ALSA)
    add_library(ALSA::alsa ALIAS PkgConfig::ALSA)
    message(STATUS "Found ALSA")
  endif()
endif()

pkg_check_modules(NCURSESW IMPORTED_TARGET QUIET ncursesw)
if(TARGET PkgConfig::NCURSESW)
  kr_sanitize_pkgconfig_target(PkgConfig::NCURSESW)
  add_library(Ncurses::w ALIAS PkgConfig::NCURSESW)
  message(STATUS "Found ncursesw")
endif()

pkg_check_modules(OGG IMPORTED_TARGET QUIET ogg)
if(TARGET PkgConfig::OGG)
  kr_sanitize_pkgconfig_target(PkgConfig::OGG)
  add_library(Ogg::ogg ALIAS PkgConfig::OGG)
  message(STATUS "Found Ogg")
endif()

pkg_check_modules(SAMPLERATE IMPORTED_TARGET QUIET samplerate)
if(TARGET PkgConfig::SAMPLERATE)
  kr_sanitize_pkgconfig_target(PkgConfig::SAMPLERATE)
  add_library(SampleRate::samplerate ALIAS PkgConfig::SAMPLERATE)
  message(STATUS "Found libsamplerate")
endif()

# ==================== AVAHI ====================
if(ENABLE_AVAHI)
  pkg_check_modules(AVAHI IMPORTED_TARGET QUIET avahi-client)
  if(TARGET PkgConfig::AVAHI)
    kr_sanitize_pkgconfig_target(PkgConfig::AVAHI)
    add_library(Avahi::client ALIAS PkgConfig::AVAHI)
    message(STATUS "Found Avahi")
  else()
    message(WARNING "Avahi requested but not found - service discovery will be unavailable")
  endif()
endif()

# ==================== SDR DRIVERS ====================
if(ENABLE_SDR_DRIVERS)
  message(STATUS "Looking for SDR drivers...")
  
  # Airspy R2/Mini
  pkg_check_modules(AIRSPY IMPORTED_TARGET QUIET libairspy)
  if(TARGET PkgConfig::AIRSPY)
    kr_sanitize_pkgconfig_target(PkgConfig::AIRSPY)
    kr_validate_header(PkgConfig::AIRSPY "libairspy/airspy.h")
    if(PkgConfig::AIRSPY_HEADER_VALID)
      add_library(Airspy::airspy ALIAS PkgConfig::AIRSPY)
      message(STATUS "  Found Airspy driver")
    else()
      message(STATUS "  Airspy library found but headers not usable - skipping")
    endif()
  endif()
  
  # Airspy HF+
  pkg_check_modules(AIRSPYHF IMPORTED_TARGET QUIET libairspyhf)
  if(TARGET PkgConfig::AIRSPYHF)
    kr_sanitize_pkgconfig_target(PkgConfig::AIRSPYHF)
    kr_validate_header(PkgConfig::AIRSPYHF "libairspyhf/airspyhf.h")
    if(PkgConfig::AIRSPYHF_HEADER_VALID)
      add_library(Airspy::airspyhf ALIAS PkgConfig::AIRSPYHF)
      message(STATUS "  Found AirspyHF driver")
    else()
      message(STATUS "  AirspyHF library found but headers not usable - skipping")
    endif()
  endif()
  
  # RTL-SDR
  pkg_check_modules(RTLSDR IMPORTED_TARGET QUIET librtlsdr)
  if(TARGET PkgConfig::RTLSDR)
    kr_sanitize_pkgconfig_target(PkgConfig::RTLSDR)
    kr_validate_header(PkgConfig::RTLSDR "rtl-sdr.h")
    if(PkgConfig::RTLSDR_HEADER_VALID)
      add_library(RTLSDR::rtlsdr ALIAS PkgConfig::RTLSDR)
      message(STATUS "  Found RTL-SDR driver")
    else()
      message(STATUS "  RTL-SDR library found but headers not usable - skipping")
    endif()
  endif()
  
  # HackRF
  pkg_check_modules(HACKRF IMPORTED_TARGET QUIET libhackrf)
  if(TARGET PkgConfig::HACKRF)
    kr_sanitize_pkgconfig_target(PkgConfig::HACKRF)
    kr_validate_header(PkgConfig::HACKRF "libhackrf/hackrf.h")
    if(PkgConfig::HACKRF_HEADER_VALID)
      add_library(HackRF::hackrf ALIAS PkgConfig::HACKRF)
      message(STATUS "  Found HackRF driver")
    else()
      message(STATUS "  HackRF library found but headers not usable - skipping")
    endif()
  endif()
  
  if(NOT TARGET Airspy::airspy AND NOT TARGET Airspy::airspyhf AND 
     NOT TARGET RTLSDR::rtlsdr AND NOT TARGET HackRF::hackrf)
    message(STATUS "  No SDR drivers found - radiod will run without hardware support")
  endif()
endif()

# ==================== LIBUSB ====================
pkg_check_modules(LIBUSB IMPORTED_TARGET QUIET libusb-1.0)

if(TARGET PkgConfig::LIBUSB)
  kr_sanitize_pkgconfig_target(PkgConfig::LIBUSB)
  add_library(LibUSB::libusb-1.0 ALIAS PkgConfig::LIBUSB)
  set(LIBUSB_PROVIDER "pkg-config")
  message(STATUS "Found libusb-1.0 (via pkg-config)")
elseif(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
  find_path(LIBUSB_INCLUDE_DIR NAMES libusb.h PATHS /usr/include)
  find_library(LIBUSB_LIBRARY NAMES usb PATHS /usr/lib /lib)
  if(LIBUSB_INCLUDE_DIR AND LIBUSB_LIBRARY)
    add_library(LibUSB::libusb-1.0 UNKNOWN IMPORTED)
    set_target_properties(LibUSB::libusb-1.0 PROPERTIES
      IMPORTED_LOCATION             "${LIBUSB_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${LIBUSB_INCLUDE_DIR}")
    set(LIBUSB_PROVIDER "freebsd-base")
    message(STATUS "Found libusb-1.0 (FreeBSD base)")
  endif()
else()
  message(STATUS "libusb-1.0 not found - USB-based SDR devices will not be available")
endif()

# ==================== LIBUSB FEATURE PROBE ====================
if(TARGET LibUSB::libusb-1.0)
  set(_KR_SAVE_REQ_INCS ${CMAKE_REQUIRED_INCLUDES})
  set(CMAKE_REQUIRED_INCLUDES
      ${CMAKE_REQUIRED_INCLUDES}
      ${LIBUSB_INCLUDE_DIRS}
      ${LIBUSB_INCLUDE_DIR}
      ${CMAKE_SOURCE_DIR}/src)
  
  set(HAVE_LIBUSB_GET_STRING_DESCRIPTOR 0)
  check_symbol_exists(libusb_get_string_descriptor "compat_libusb.h" KR_HAVE_LUSB_GSD)
  if(KR_HAVE_LUSB_GSD)
    set(HAVE_LIBUSB_GET_STRING_DESCRIPTOR 1)
    message(STATUS "libusb has libusb_get_string_descriptor")
  endif()
  
  set(HAVE_LIBUSB_GET_STRING_DESCRIPTOR
      ${HAVE_LIBUSB_GET_STRING_DESCRIPTOR}
      CACHE INTERNAL "libusb provides libusb_get_string_descriptor()")
  
  set(CMAKE_REQUIRED_INCLUDES ${_KR_SAVE_REQ_INCS})
endif()

# ==================== ICONV ====================
set(_save_req_libs "${CMAKE_REQUIRED_LIBRARIES}")
set(CMAKE_REQUIRED_LIBRARIES "")

check_c_source_compiles([=[
  #include <iconv.h>
  int main(void){
    iconv_t cd = iconv_open("UTF-8", "UTF-8");
    return (cd == (iconv_t)-1);
  }
]=] Iconv_IS_BUILT_IN)

set(CMAKE_REQUIRED_LIBRARIES "${_save_req_libs}")

if(Iconv_IS_BUILT_IN)
  set(ICONV_FOUND TRUE CACHE INTERNAL "")
  set(ICONV_INCLUDE_DIR "" CACHE INTERNAL "")
  set(ICONV_LIBRARIES "" CACHE INTERNAL "")
  message(STATUS "Found iconv (built-in)")
else()
  find_library(ICONV_LIBRARIES NAMES iconv)
  find_path(ICONV_INCLUDE_DIR NAMES iconv.h PATHS /usr/local/include /usr/include)
  if(ICONV_LIBRARIES AND ICONV_INCLUDE_DIR)
    set(ICONV_FOUND TRUE CACHE INTERNAL "")
    message(STATUS "Found iconv: ${ICONV_LIBRARIES}")
  else()
    set(ICONV_FOUND FALSE CACHE INTERNAL "")
    message(WARNING "iconv not found - may need to install libiconv")
  endif()
endif()

# ==================== HELPER FUNCTION ====================
function(kr_link_common_deps tgt)
  find_package(Threads REQUIRED)
  target_link_libraries(${tgt} PUBLIC Threads::Threads m)
  
  if(TARGET FFTW::fftw3f)
    target_link_libraries(${tgt} PUBLIC FFTW::fftw3f)
  endif()
  if(TARGET FFTW::fftw3f_threads)
    target_link_libraries(${tgt} PUBLIC FFTW::fftw3f_threads)
  endif()
  if(TARGET PortAudio::portaudio)
    target_link_libraries(${tgt} PUBLIC PortAudio::portaudio)
  endif()
  if(TARGET ALSA::alsa)
    target_link_libraries(${tgt} PUBLIC ALSA::alsa)
  endif()
  if(TARGET Ncurses::w)
    target_link_libraries(${tgt} PUBLIC Ncurses::w)
  endif()
  if(TARGET Ogg::ogg)
    target_link_libraries(${tgt} PUBLIC Ogg::ogg)
  endif()
  if(TARGET SampleRate::samplerate)
    target_link_libraries(${tgt} PUBLIC SampleRate::samplerate)
  endif()
  if(TARGET Avahi::client)
    target_link_libraries(${tgt} PUBLIC Avahi::client)
  endif()
endfunction()
