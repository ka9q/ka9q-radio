# cmake/FindDeps.cmake
include_guard(GLOBAL)
find_package(PkgConfig REQUIRED)
include(CheckSymbolExists)

# -------------------- FFTW (float) --------------------
pkg_check_modules(FFTW3F IMPORTED_TARGET QUIET fftw3f)
if(TARGET PkgConfig::FFTW3F)
  add_library(FFTW::fftw3f ALIAS PkgConfig::FFTW3F)
  set(HAVE_FFTW3F TRUE)
endif()

# Threads sublib (some OSes lack a .pc for this)
pkg_check_modules(FFTW3F_THR IMPORTED_TARGET QUIET fftw3f_threads)
if(TARGET PkgConfig::FFTW3F_THR)
  add_library(FFTW::fftw3f_threads ALIAS PkgConfig::FFTW3F_THR)
  set(HAVE_FFTW3F_THREADS TRUE)
else()
  find_library(FFTW3F_THREADS_LIBRARY NAMES fftw3f_threads)
  if(FFTW3F_THREADS_LIBRARY)
    add_library(FFTW::fftw3f_threads UNKNOWN IMPORTED)
    set_target_properties(FFTW::fftw3f_threads PROPERTIES
      IMPORTED_LOCATION "${FFTW3F_THREADS_LIBRARY}")
    set(HAVE_FFTW3F_THREADS TRUE)
  endif()
endif()

# -------------------- opus --------------------
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
  pkg_check_modules(OPUS QUIET opus)
endif()

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

# Derive library dirs from absolute lib paths if pkg-config didn't set them
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

# Imported interface target for cleaner linking
if(OPUS_FOUND AND NOT TARGET opus::opus)
  add_library(opus::opus INTERFACE IMPORTED)
  target_include_directories(opus::opus INTERFACE ${OPUS_INCLUDE_DIRS})
  if(OPUS_LIBRARY_DIRS)
    target_link_directories(opus::opus INTERFACE ${OPUS_LIBRARY_DIRS})
  endif()
  target_link_libraries(opus::opus INTERFACE ${OPUS_LIBRARIES})
endif()

# Cache for subdirs
set(OPUS_FOUND ${OPUS_FOUND} CACHE INTERNAL "")
set(OPUS_INCLUDE_DIRS ${OPUS_INCLUDE_DIRS} CACHE INTERNAL "")
set(OPUS_LIBRARY_DIRS ${OPUS_LIBRARY_DIRS} CACHE INTERNAL "")
set(OPUS_LIBRARIES ${OPUS_LIBRARIES} CACHE INTERNAL "")

# -------------------- libbsd (Linux usually) --------------------
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  pkg_check_modules(LIBBSD IMPORTED_TARGET QUIET libbsd)
  if(TARGET PkgConfig::libbsd)
    add_library(LibBSD::libbsd ALIAS PkgConfig::libbsd)
  endif()
endif()

# -------------------- iniparser (via pkg-config) --------------------
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
  pkg_check_modules(INIPARSER QUIET iniparser)
endif()

# Fallback search when pkg-config is missing or has no .pc for iniparser
if(NOT INIPARSER_FOUND)
  find_path(INIPARSER_INCLUDE_DIRS
            NAMES iniparser/iniparser.h iniparser.h
            PATHS
              /opt/homebrew/include
              /usr/local/include
              /usr/include)
  find_library(INIPARSER_LIBRARIES
               NAMES iniparser
               PATHS
                 /opt/homebrew/lib
                 /usr/local/lib
                 /usr/lib
                 /usr/lib/x86_64-linux-gnu
                 /usr/lib/i386-linux-gnu)
  if(INIPARSER_INCLUDE_DIRS AND INIPARSER_LIBRARIES)
    set(INIPARSER_FOUND TRUE)
  endif()
endif()

# Derive library dirs from absolute lib paths if pkg-config didn't set them
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

# Imported interface target for cleaner linking
if(INIPARSER_FOUND AND NOT TARGET iniparser::iniparser)
  add_library(iniparser::iniparser INTERFACE IMPORTED)
  target_include_directories(iniparser::iniparser INTERFACE ${INIPARSER_INCLUDE_DIRS})
  if(INIPARSER_LIBRARY_DIRS)
    target_link_directories(iniparser::iniparser INTERFACE ${INIPARSER_LIBRARY_DIRS})
  endif()
  target_link_libraries(iniparser::iniparser INTERFACE ${INIPARSER_LIBRARIES})
endif()

# Cache for subdirs
set(INIPARSER_FOUND ${INIPARSER_FOUND} CACHE INTERNAL "")
set(INIPARSER_INCLUDE_DIRS ${INIPARSER_INCLUDE_DIRS} CACHE INTERNAL "")
set(INIPARSER_LIBRARY_DIRS ${INIPARSER_LIBRARY_DIRS} CACHE INTERNAL "")
set(INIPARSER_LIBRARIES ${INIPARSER_LIBRARIES} CACHE INTERNAL "")

# -------------------- hidapi --------------------
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
  # Try all common variants
  pkg_check_modules(HIDAPI QUIET hidapi hidapi-libusb hidapi-hidraw)
endif()

if(NOT HIDAPI_FOUND)
  # Manual fallback
  find_path(HIDAPI_INCLUDE_DIRS
            NAMES hidapi/hidapi.h
            PATHS /opt/homebrew/include /usr/local/include /usr/include)

  find_library(HIDAPI_LIBRARIES
               NAMES hidapi hidapi-libusb hidapi-hidraw
               PATHS /opt/homebrew/lib /usr/local/lib /usr/lib)

  # Derive the library directory (for rpath/link path)
  if(HIDAPI_LIBRARIES)
    get_filename_component(HIDAPI_LIBRARY_DIRS "${HIDAPI_LIBRARIES}" DIRECTORY)
  endif()

  if(HIDAPI_INCLUDE_DIRS AND HIDAPI_LIBRARIES)
    set(HIDAPI_FOUND TRUE)
  endif()
endif()

# Logging for diagnostics
message(STATUS "HIDAPI lib: ${HIDAPI_LIBRARIES}")
message(STATUS "HIDAPI include: ${HIDAPI_INCLUDE_DIRS}")

# Enforce HIDAPI on macOS; optional elsewhere
set(HIDAPI_REQUIRED_FOR_RADIOD OFF)

if(APPLE)
  if(NOT HIDAPI_FOUND)
    message(FATAL_ERROR "Required dependency 'hidapi' not found on macOS. Try: brew install hidapi")
  endif()
  set(HIDAPI_REQUIRED_FOR_RADIOD ON)

elseif(UNIX AND NOT APPLE)
  if(NOT HIDAPI_FOUND)
    message(FATAL_ERROR "Required dependency 'hidapi' not found on Linux. Try: sudo apt install libhidapi-dev")
  endif()
  set(HIDAPI_REQUIRED_FOR_RADIOD ON)
endif()

# Always define imported target if found
if(HIDAPI_FOUND AND NOT TARGET hidapi::hidapi)
  add_library(hidapi::hidapi INTERFACE IMPORTED)
  target_include_directories(hidapi::hidapi INTERFACE ${HIDAPI_INCLUDE_DIRS})
  if(HIDAPI_LIBRARY_DIRS)
    target_link_directories(hidapi::hidapi INTERFACE ${HIDAPI_LIBRARY_DIRS})
  endif()
  target_link_libraries(hidapi::hidapi INTERFACE ${HIDAPI_LIBRARIES})
endif()

# Cache variables for other directories
set(HIDAPI_FOUND ${HIDAPI_FOUND} CACHE INTERNAL "")
set(HIDAPI_INCLUDE_DIRS ${HIDAPI_INCLUDE_DIRS} CACHE INTERNAL "")
set(HIDAPI_LIBRARIES ${HIDAPI_LIBRARIES} CACHE INTERNAL "")
set(HIDAPI_LIBRARY_DIRS ${HIDAPI_LIBRARY_DIRS} CACHE INTERNAL "")
set(HIDAPI_REQUIRED_FOR_RADIOD ${HIDAPI_REQUIRED_FOR_RADIOD} CACHE INTERNAL "")

# -------------------- Audio/UI libs --------------------
pkg_check_modules(PORTAUDIO IMPORTED_TARGET QUIET portaudio-2.0)
if(TARGET PkgConfig::PORTAUDIO)
  add_library(PortAudio::portaudio ALIAS PkgConfig::PORTAUDIO)
endif()

if(NOT APPLE)
  pkg_check_modules(ALSA IMPORTED_TARGET QUIET alsa)
  if(TARGET PkgConfig::ALSA)
    add_library(ALSA::alsa ALIAS PkgConfig::ALSA)
  endif()
endif()

pkg_check_modules(NCURSESW IMPORTED_TARGET QUIET ncursesw)
if(TARGET PkgConfig::NCURSESW)
  add_library(Ncurses::w ALIAS PkgConfig::NCURSESW)
endif()

pkg_check_modules(OGG IMPORTED_TARGET QUIET ogg)
if(TARGET PkgConfig::OGG)
  add_library(Ogg::ogg ALIAS PkgConfig::OGG)
endif()

pkg_check_modules(SAMPLERATE IMPORTED_TARGET QUIET samplerate)
if(TARGET PkgConfig::SAMPLERATE)
  add_library(SampleRate::samplerate ALIAS PkgConfig::SAMPLERATE)
endif()

# -------------------- Avahi --------------------
pkg_check_modules(AVAHI IMPORTED_TARGET QUIET avahi-client)
if(TARGET PkgConfig::AVAHI)
  add_library(Avahi::client ALIAS PkgConfig::AVAHI)
endif()

# -------------------- SDR drivers via pkg-config --------------------
pkg_check_modules(AIRSPY     IMPORTED_TARGET QUIET libairspy)
pkg_check_modules(AIRSPYHF   IMPORTED_TARGET QUIET libairspyhf)
pkg_check_modules(RTLSDR     IMPORTED_TARGET QUIET librtlsdr)
pkg_check_modules(HACKRF     IMPORTED_TARGET QUIET libhackrf)

# Create canonical aliases only if found
if(TARGET PkgConfig::libairspy)
  add_library(Airspy::airspy     ALIAS PkgConfig::libairspy)
endif()
if(TARGET PkgConfig::libairspyhf)
  add_library(Airspy::airspyhf   ALIAS PkgConfig::libairspyhf)
endif()
if(TARGET PkgConfig::librtlsdr)
  add_library(RTLSDR::rtlsdr     ALIAS PkgConfig::librtlsdr)
endif()
if(TARGET PkgConfig::libhackrf)
  add_library(HackRF::hackrf     ALIAS PkgConfig::libhackrf)
endif()

# -------------------- libusb (Linux/macOS via pkg; FreeBSD via base) --------------------
pkg_check_modules(LIBUSB IMPORTED_TARGET QUIET libusb-1.0)
if(TARGET PkgConfig::libusb-1.0)
  add_library(LibUSB::libusb-1.0 ALIAS PkgConfig::libusb-1.0)
  set(LIBUSB_PROVIDER "pkg-config")
elseif(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
  find_path(LIBUSB_INCLUDE_DIR NAMES libusb.h PATHS /usr/include)
  find_library(LIBUSB_LIBRARY NAMES usb PATHS /usr/lib /lib)
  if(LIBUSB_INCLUDE_DIR AND LIBUSB_LIBRARY)
    add_library(LibUSB::libusb-1.0 UNKNOWN IMPORTED)
    set_target_properties(LibUSB::libusb-1.0 PROPERTIES
      IMPORTED_LOCATION             "${LIBUSB_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${LIBUSB_INCLUDE_DIR}")
    set(LIBUSB_PROVIDER "freebsd-base")
  endif()
endif()

# -------------------- libusb feature probe (get_string_descriptor) --------------------
# Use the same include dirs the target will compile with; also add src/ for compat_libusb.h
set(_KR_SAVE_REQ_INCS ${CMAKE_REQUIRED_INCLUDES})
set(CMAKE_REQUIRED_INCLUDES
    ${CMAKE_REQUIRED_INCLUDES}
    ${LIBUSB_INCLUDE_DIRS}
    ${LIBUSB_INCLUDE_DIR}           # FreeBSD base case
    ${CMAKE_SOURCE_DIR}/src)        # compat_libusb.h

set(HAVE_LIBUSB_GET_STRING_DESCRIPTOR 0)
# Probe via our portable header so the path always matches build usage
check_symbol_exists(libusb_get_string_descriptor "compat_libusb.h" KR_HAVE_LUSB_GSD)
if(KR_HAVE_LUSB_GSD)
  set(HAVE_LIBUSB_GET_STRING_DESCRIPTOR 1)
endif()

# export to parent scope for src/CMakeLists.txt to consume
set(HAVE_LIBUSB_GET_STRING_DESCRIPTOR
    ${HAVE_LIBUSB_GET_STRING_DESCRIPTOR}
    CACHE INTERNAL "libusb provides libusb_get_string_descriptor()")

set(CMAKE_REQUIRED_INCLUDES ${_KR_SAVE_REQ_INCS})

# --- iconv detection ---
include(CheckCSourceCompiles)

# Probe whether iconv links from libc without extra libraries
set(_save_req_libs "${CMAKE_REQUIRED_LIBRARIES}")
set(CMAKE_REQUIRED_LIBRARIES "")  # try plain libc first
check_c_source_compiles([=[
  #include <iconv.h>
  int main(void){
    iconv_t cd = iconv_open("UTF-8", "UTF-8");
    return (cd == (iconv_t)-1);
  }
]=] Iconv_IS_BUILT_IN)
set(CMAKE_REQUIRED_LIBRARIES "${_save_req_libs}")

if(Iconv_IS_BUILT_IN)
  # Don't set ICONV_INCLUDE_DIR/ICONV_LIBRARIES in this case
  set(ICONV_FOUND TRUE CACHE INTERNAL "")
  set(ICONV_INCLUDE_DIR "" CACHE INTERNAL "")
  set(ICONV_LIBRARIES "" CACHE INTERNAL "")
else()
  # Fall back to finding libiconv from ports/homebrew/etc
  find_library(ICONV_LIBRARIES NAMES iconv)
  find_path(ICONV_INCLUDE_DIR NAMES iconv.h PATHS /usr/local/include /usr/include)
  if(ICONV_LIBRARIES AND ICONV_INCLUDE_DIR)
    set(ICONV_FOUND TRUE CACHE INTERNAL "")
  else()
    set(ICONV_FOUND FALSE CACHE INTERNAL "")
  endif()
endif()

# -------------------- Helper to link common deps (CALL LATER) --------------------
function(kr_link_common_deps tgt)
  find_package(Threads REQUIRED)
  target_link_libraries(${tgt} PUBLIC Threads::Threads m)

  if(TARGET FFTW::fftw3f)
    target_link_libraries(${tgt} PUBLIC FFTW::fftw3f)          
  endif()
  if(TARGET FFTW::fftw3f_threads)
    target_link_libraries(${tgt} PUBLIC FFTW::fftw3f_threads)
  endif()
  if(TARGET Iniparser::iniparser)
    target_link_libraries(${tgt} PUBLIC Iniparser::iniparser)
  endif()
  if(TARGET Opus::opus)
    target_link_libraries(${tgt} PUBLIC Opus::opus)
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

