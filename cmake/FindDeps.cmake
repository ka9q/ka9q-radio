# cmake/FindDeps.cmake
# Dependency discovery and imported target creation
# Uses deferred error reporting - checks all dependencies first, then fails if any are missing
include_guard(GLOBAL)

find_package(PkgConfig REQUIRED)
include(CheckSymbolExists)
include(CheckCSourceCompiles)

# Track missing required dependencies
set(KR_MISSING_DEPS "")
set(KR_MISSING_OPTIONAL_DEPS "")

# Helper macro to add to missing deps list
macro(kr_mark_missing dep_name is_required message)
  if(${is_required})
    list(APPEND KR_MISSING_DEPS "${dep_name}")
    message(WARNING "REQUIRED: ${dep_name} not found - ${message}")
  else()
    list(APPEND KR_MISSING_OPTIONAL_DEPS "${dep_name}")
    message(STATUS "OPTIONAL: ${dep_name} not found - ${message}")
  endif()
endmacro()

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
        message(STATUS "Removing non-existent include path from ${target_name}: ${_inc}")
      endif()
    endforeach()
    if(_clean_includes)
      set_target_properties(${target_name} PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${_clean_includes}")
    else()
      # No valid includes left, unset the property
      set_property(TARGET ${target_name} PROPERTY INTERFACE_INCLUDE_DIRECTORIES "")
    endif()
  endif()
endfunction()

# ==================== FFTW (FLOAT) ====================
pkg_check_modules(FFTW3F IMPORTED_TARGET QUIET fftw3f)
if(TARGET PkgConfig::FFTW3F)
  kr_sanitize_pkgconfig_target(PkgConfig::FFTW3F)
  add_library(FFTW::fftw3f ALIAS PkgConfig::FFTW3F)
  set(HAVE_FFTW3F TRUE)
  message(STATUS "✓ Found FFTW3f: ${FFTW3F_LIBRARIES}")
else()
  kr_mark_missing("FFTW3f" TRUE "Install with: brew install fftw (macOS) / apt install libfftw3-dev (Debian/Ubuntu) / pkg install fftw3 (FreeBSD)")
endif()

# FFTW threads sublib (some OSes lack a .pc for this)
pkg_check_modules(FFTW3F_THR IMPORTED_TARGET QUIET fftw3f_threads)
if(TARGET PkgConfig::FFTW3F_THR)
  kr_sanitize_pkgconfig_target(PkgConfig::FFTW3F_THR)
  add_library(FFTW::fftw3f_threads ALIAS PkgConfig::FFTW3F_THR)
  set(HAVE_FFTW3F_THREADS TRUE)
  message(STATUS "✓ Found FFTW3f threads support")
else()
  # Fallback: try to find it manually
  find_library(FFTW3F_THREADS_LIBRARY NAMES fftw3f_threads)
  if(FFTW3F_THREADS_LIBRARY)
    add_library(FFTW::fftw3f_threads UNKNOWN IMPORTED)
    set_target_properties(FFTW::fftw3f_threads PROPERTIES
      IMPORTED_LOCATION "${FFTW3F_THREADS_LIBRARY}")
    set(HAVE_FFTW3F_THREADS TRUE)
    message(STATUS "✓ Found FFTW3f threads (manual): ${FFTW3F_THREADS_LIBRARY}")
  else()
    message(STATUS "  FFTW3f threads not found - single-threaded FFTW only")
  endif()
endif()

# ==================== OPUS ====================
# Try pkg-config first
pkg_check_modules(OPUS QUIET opus)

if(NOT OPUS_FOUND)
  # Manual fallback for systems without opus.pc
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

# Derive library dirs from absolute lib paths if needed
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

# Create imported interface target
if(OPUS_FOUND AND NOT TARGET opus::opus)
  add_library(opus::opus INTERFACE IMPORTED)
  target_include_directories(opus::opus INTERFACE ${OPUS_INCLUDE_DIRS})
  if(OPUS_LIBRARY_DIRS)
    target_link_directories(opus::opus INTERFACE ${OPUS_LIBRARY_DIRS})
  endif()
  target_link_libraries(opus::opus INTERFACE ${OPUS_LIBRARIES})
  message(STATUS "✓ Found Opus: ${OPUS_LIBRARIES}")
else()
  kr_mark_missing("Opus" TRUE "Install with: brew install opus (macOS) / apt install libopus-dev (Debian/Ubuntu) / pkg install opus (FreeBSD)")
endif()

# Cache for subdirs
set(OPUS_FOUND ${OPUS_FOUND} CACHE INTERNAL "")
set(OPUS_INCLUDE_DIRS ${OPUS_INCLUDE_DIRS} CACHE INTERNAL "")
set(OPUS_LIBRARY_DIRS ${OPUS_LIBRARY_DIRS} CACHE INTERNAL "")
set(OPUS_LIBRARIES ${OPUS_LIBRARIES} CACHE INTERNAL "")

# ==================== LIBBSD (LINUX) ====================
# Linux needs libbsd for strlcpy and other BSD functions
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  pkg_check_modules(LIBBSD IMPORTED_TARGET QUIET libbsd)
  if(TARGET PkgConfig::LIBBSD)
    kr_sanitize_pkgconfig_target(PkgConfig::LIBBSD)
    add_library(LibBSD::libbsd ALIAS PkgConfig::LIBBSD)
    message(STATUS "✓ Found libbsd: ${LIBBSD_LIBRARIES}")
  endif()
endif()

# ==================== INIPARSER ====================
# Try pkg-config first
pkg_check_modules(INIPARSER QUIET iniparser)

if(NOT INIPARSER_FOUND)
  # Manual fallback
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

# Derive library dirs if needed
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

# Create imported interface target
if(INIPARSER_FOUND AND NOT TARGET iniparser::iniparser)
  add_library(iniparser::iniparser INTERFACE IMPORTED)
  target_include_directories(iniparser::iniparser INTERFACE ${INIPARSER_INCLUDE_DIRS})
  if(INIPARSER_LIBRARY_DIRS)
    target_link_directories(iniparser::iniparser INTERFACE ${INIPARSER_LIBRARY_DIRS})
  endif()
  target_link_libraries(iniparser::iniparser INTERFACE ${INIPARSER_LIBRARIES})
  message(STATUS "✓ Found iniparser: ${INIPARSER_LIBRARIES}")
else()
  kr_mark_missing("iniparser" TRUE "Install with: brew install iniparser (macOS) / apt install libiniparser-dev (Debian/Ubuntu) / pkg install iniparser (FreeBSD)")
endif()

# Cache for subdirs
set(INIPARSER_FOUND ${INIPARSER_FOUND} CACHE INTERNAL "")
set(INIPARSER_INCLUDE_DIRS ${INIPARSER_INCLUDE_DIRS} CACHE INTERNAL "")
set(INIPARSER_LIBRARY_DIRS ${INIPARSER_LIBRARY_DIRS} CACHE INTERNAL "")
set(INIPARSER_LIBRARIES ${INIPARSER_LIBRARIES} CACHE INTERNAL "")

# ==================== HIDAPI ====================
# Try all common variants via pkg-config
pkg_check_modules(HIDAPI QUIET hidapi hidapi-libusb hidapi-hidraw)

if(NOT HIDAPI_FOUND)
  # Manual fallback
  find_path(HIDAPI_INCLUDE_DIRS
            NAMES hidapi/hidapi.h
            PATHS /opt/homebrew/include /usr/local/include /usr/include)
  find_library(HIDAPI_LIBRARIES
               NAMES hidapi hidapi-libusb hidapi-hidraw
               PATHS /opt/homebrew/lib /usr/local/lib /usr/lib)
  
  # Derive library directory
  if(HIDAPI_LIBRARIES)
    get_filename_component(HIDAPI_LIBRARY_DIRS "${HIDAPI_LIBRARIES}" DIRECTORY)
  endif()
  
  if(HIDAPI_INCLUDE_DIRS AND HIDAPI_LIBRARIES)
    set(HIDAPI_FOUND TRUE)
  endif()
endif()

# HIDAPI is required on macOS and Linux for HID-based SDR devices
set(HIDAPI_REQUIRED_FOR_RADIOD OFF)

if(APPLE)
  if(NOT HIDAPI_FOUND)
    kr_mark_missing("HIDAPI" TRUE "Install with: brew install hidapi")
  else()
    message(STATUS "✓ Found HIDAPI: ${HIDAPI_LIBRARIES}")
  endif()
  set(HIDAPI_REQUIRED_FOR_RADIOD ON)
  
elseif(UNIX AND NOT APPLE)  # Linux/BSD
  if(NOT HIDAPI_FOUND)
    kr_mark_missing("HIDAPI" FALSE "Install with: apt install libhidapi-dev (Debian/Ubuntu) / pkg install hidapi (FreeBSD)")
  else()
    set(HIDAPI_REQUIRED_FOR_RADIOD ON)
    message(STATUS "✓ Found HIDAPI: ${HIDAPI_LIBRARIES}")
  endif()
endif()

# Create imported target if found
if(HIDAPI_FOUND AND NOT TARGET hidapi::hidapi)
  add_library(hidapi::hidapi INTERFACE IMPORTED)
  target_include_directories(hidapi::hidapi INTERFACE ${HIDAPI_INCLUDE_DIRS})
  if(HIDAPI_LIBRARY_DIRS)
    target_link_directories(hidapi::hidapi INTERFACE ${HIDAPI_LIBRARY_DIRS})
  endif()
  target_link_libraries(hidapi::hidapi INTERFACE ${HIDAPI_LIBRARIES})
endif()

# Cache variables
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
  message(STATUS "✓ Found PortAudio")
endif()

if(NOT APPLE)
  pkg_check_modules(ALSA IMPORTED_TARGET QUIET alsa)
  if(TARGET PkgConfig::ALSA)
    kr_sanitize_pkgconfig_target(PkgConfig::ALSA)
    add_library(ALSA::alsa ALIAS PkgConfig::ALSA)
    message(STATUS "✓ Found ALSA")
  endif()
endif()

pkg_check_modules(NCURSESW IMPORTED_TARGET QUIET ncursesw)
if(TARGET PkgConfig::NCURSESW)
  kr_sanitize_pkgconfig_target(PkgConfig::NCURSESW)
  add_library(Ncurses::w ALIAS PkgConfig::NCURSESW)
  message(STATUS "✓ Found ncursesw")
endif()

pkg_check_modules(OGG IMPORTED_TARGET QUIET ogg)
if(TARGET PkgConfig::OGG)
  kr_sanitize_pkgconfig_target(PkgConfig::OGG)
  add_library(Ogg::ogg ALIAS PkgConfig::OGG)
  message(STATUS "✓ Found Ogg")
endif()

pkg_check_modules(SAMPLERATE IMPORTED_TARGET QUIET samplerate)
if(TARGET PkgConfig::SAMPLERATE)
  kr_sanitize_pkgconfig_target(PkgConfig::SAMPLERATE)
  add_library(SampleRate::samplerate ALIAS PkgConfig::SAMPLERATE)
  message(STATUS "✓ Found libsamplerate")
endif()

# ==================== AVAHI ====================
if(ENABLE_AVAHI)
  pkg_check_modules(AVAHI IMPORTED_TARGET QUIET avahi-client)
  if(TARGET PkgConfig::AVAHI)
    kr_sanitize_pkgconfig_target(PkgConfig::AVAHI)
    add_library(Avahi::client ALIAS PkgConfig::AVAHI)
    message(STATUS "✓ Found Avahi")
  else()
    message(STATUS "  Avahi requested but not found - service discovery will be unavailable")
  endif()
endif()

# ==================== SDR DRIVERS ====================
if(ENABLE_SDR_DRIVERS)
  message(STATUS "Looking for SDR drivers...")
  
  pkg_check_modules(AIRSPY IMPORTED_TARGET QUIET libairspy)
  pkg_check_modules(AIRSPYHF IMPORTED_TARGET QUIET libairspyhf)
  pkg_check_modules(RTLSDR IMPORTED_TARGET QUIET librtlsdr)
  pkg_check_modules(HACKRF IMPORTED_TARGET QUIET libhackrf)
  
  # Sanitize all SDR driver targets (Homebrew stale path workaround)
  if(TARGET PkgConfig::AIRSPY)
    kr_sanitize_pkgconfig_target(PkgConfig::AIRSPY)
  endif()
  if(TARGET PkgConfig::AIRSPYHF)
    kr_sanitize_pkgconfig_target(PkgConfig::AIRSPYHF)
  endif()
  if(TARGET PkgConfig::RTLSDR)
    kr_sanitize_pkgconfig_target(PkgConfig::RTLSDR)
  endif()
  if(TARGET PkgConfig::HACKRF)
    kr_sanitize_pkgconfig_target(PkgConfig::HACKRF)
  endif()
  
  # Create canonical aliases
  if(TARGET PkgConfig::AIRSPY)
    add_library(Airspy::airspy ALIAS PkgConfig::AIRSPY)
    message(STATUS "  ✓ Found Airspy driver")
  endif()
  if(TARGET PkgConfig::AIRSPYHF)
    add_library(Airspy::airspyhf ALIAS PkgConfig::AIRSPYHF)
    message(STATUS "  ✓ Found AirspyHF driver")
  endif()
  if(TARGET PkgConfig::RTLSDR)
    add_library(RTLSDR::rtlsdr ALIAS PkgConfig::RTLSDR)
    message(STATUS "  ✓ Found RTL-SDR driver")
  endif()
  if(TARGET PkgConfig::HACKRF)
    add_library(HackRF::hackrf ALIAS PkgConfig::HACKRF)
    message(STATUS "  ✓ Found HackRF driver")
  endif()
endif()

# ==================== LIBUSB ====================
# Linux/macOS use pkg-config; FreeBSD has it in base
pkg_check_modules(LIBUSB IMPORTED_TARGET QUIET libusb-1.0)

if(TARGET PkgConfig::LIBUSB)
  kr_sanitize_pkgconfig_target(PkgConfig::LIBUSB)
  add_library(LibUSB::libusb-1.0 ALIAS PkgConfig::LIBUSB)
  set(LIBUSB_PROVIDER "pkg-config")
  message(STATUS "✓ Found libusb-1.0 (via pkg-config)")
  
elseif(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
  # FreeBSD has libusb in base system
  find_path(LIBUSB_INCLUDE_DIR NAMES libusb.h PATHS /usr/include)
  find_library(LIBUSB_LIBRARY NAMES usb PATHS /usr/lib /lib)
  
  if(LIBUSB_INCLUDE_DIR AND LIBUSB_LIBRARY)
    add_library(LibUSB::libusb-1.0 UNKNOWN IMPORTED)
    set_target_properties(LibUSB::libusb-1.0 PROPERTIES
      IMPORTED_LOCATION             "${LIBUSB_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${LIBUSB_INCLUDE_DIR}")
    set(LIBUSB_PROVIDER "freebsd-base")
    message(STATUS "✓ Found libusb-1.0 (FreeBSD base)")
  endif()
else()
  message(STATUS "  libusb-1.0 not found - USB-based SDR devices will not be available")
endif()

# ==================== LIBUSB FEATURE PROBE ====================
# Check if libusb_get_string_descriptor is available
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
    message(STATUS "  libusb has libusb_get_string_descriptor")
  endif()
  
  set(HAVE_LIBUSB_GET_STRING_DESCRIPTOR
      ${HAVE_LIBUSB_GET_STRING_DESCRIPTOR}
      CACHE INTERNAL "libusb provides libusb_get_string_descriptor()")
  
  set(CMAKE_REQUIRED_INCLUDES ${_KR_SAVE_REQ_INCS})
endif()

# ==================== ICONV ====================
# Check if iconv is built into libc or needs external library
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
  message(STATUS "✓ Found iconv (built-in)")
else()
  # Need external libiconv
  find_library(ICONV_LIBRARIES NAMES iconv)
  find_path(ICONV_INCLUDE_DIR NAMES iconv.h PATHS /usr/local/include /usr/include)
  
  if(ICONV_LIBRARIES AND ICONV_INCLUDE_DIR)
    set(ICONV_FOUND TRUE CACHE INTERNAL "")
    message(STATUS "✓ Found iconv: ${ICONV_LIBRARIES}")
  else()
    set(ICONV_FOUND FALSE CACHE INTERNAL "")
    message(STATUS "  iconv not found - may need to install libiconv")
  endif()
endif()

# ==================== HELPER FUNCTION ====================
# Link common dependencies to a target
function(kr_link_common_deps tgt)
  find_package(Threads REQUIRED)
  target_link_libraries(${tgt} PUBLIC Threads::Threads m)

  # Link optional libraries if found (use correct lowercase target names!)

  # --- OPUS ---
  if(TARGET opus::opus)
    # Use PUBLIC linking to pass required libraries through the static archive
    target_link_libraries(${tgt} PUBLIC opus::opus)
  else()
    # FALLBACK: If target doesn't propagate libraries correctly (e.g., static link issue)
    # We must explicitly use the discovered library variables.
    if(OPUS_LIBRARIES)
        target_link_libraries(${tgt} PUBLIC ${OPUS_LIBRARIES})
    endif()
  endif()

  # --- FFTW3F ---
  if(TARGET FFTW::fftw3f)
    target_link_libraries(${tgt} PUBLIC FFTW::fftw3f)
  else()
    if(FFTW3F_LIBRARIES)
      target_link_libraries(${tgt} PUBLIC ${FFTW3F_LIBRARIES})
    endif()
  endif()

  # --- FFTW3F THREADS ---
  if(TARGET FFTW::fftw3f_threads)
    target_link_libraries(${tgt} PUBLIC FFTW::fftw3f_threads)
  else()
    if(FFTW3F_THREADS_LIBRARY) # Note: this variable is the manual fallback
      target_link_libraries(${tgt} PUBLIC ${FFTW3F_THREADS_LIBRARY})
    endif()
  endif()
  
  # --- INIPARSER ---
  if(TARGET iniparser::iniparser)
    target_link_libraries(${tgt} PUBLIC iniparser::iniparser)
  else()
    if(INIPARSER_LIBRARIES)
      target_link_libraries(${tgt} PUBLIC ${INIPARSER_LIBRARIES})
    endif()
  endif()

  # --- PORTAUDIO ---
  if(TARGET PortAudio::portaudio)
    target_link_libraries(${tgt} PUBLIC PortAudio::portaudio)
  else()
    if(PORTAUDIO_LIBRARIES)
      target_link_libraries(${tgt} PUBLIC ${PORTAUDIO_LIBRARIES})
    endif()
  endif()

  # --- ALSA ---
  if(TARGET ALSA::alsa)
    target_link_libraries(${tgt} PUBLIC ALSA::alsa)
  else()
    if(ALSA_LIBRARIES)
      target_link_libraries(${tgt} PUBLIC ${ALSA_LIBRARIES})
    endif()
  endif()

  # --- NCURSESW ---
  if(TARGET Ncurses::w)
    target_link_libraries(${tgt} PUBLIC Ncurses::w)
  else()
    if(NCURSESW_LIBRARIES)
      target_link_libraries(${tgt} PUBLIC ${NCURSESW_LIBRARIES})
    endif()
  endif()

  # --- OGG ---
  if(TARGET Ogg::ogg)
    target_link_libraries(${tgt} PUBLIC Ogg::ogg)
  else()
    if(OGG_LIBRARIES)
      target_link_libraries(${tgt} PUBLIC ${OGG_LIBRARIES})
    endif()
  endif()

  # --- SAMPLERATE ---
  if(TARGET SampleRate::samplerate)
    target_link_libraries(${tgt} PUBLIC SampleRate::samplerate)
  else()
    if(SAMPLERATE_LIBRARIES)
      target_link_libraries(${tgt} PUBLIC ${SAMPLERATE_LIBRARIES})
    endif()
  endif()

  # --- AVAHI ---
  if(TARGET Avahi::client)
    target_link_libraries(${tgt} PUBLIC Avahi::client)
  else()
    if(AVAHI_LIBRARIES)
      target_link_libraries(${tgt} PUBLIC ${AVAHI_LIBRARIES})
    endif()
  endif()
endfunction()

# ==================== HELPER FUNCTION ====================
# Add explicit include directories for targets whose headers fail #if __has_include checks
function(kr_add_compat_includes tgt)
  # NOTE: These explicit includes are necessary for targets (like static libs or modules)
  # where transitive INTERFACE_INCLUDE_DIRECTORIES are not processed early enough
  # for the C preprocessor's #if __has_include checks to succeed.
  
  # Opus
  if(OPUS_FOUND AND OPUS_INCLUDE_DIRS)
    target_include_directories(${tgt} PUBLIC ${OPUS_INCLUDE_DIRS})
    message(STATUS "Patch: Added Opus includes to target ${tgt}")
  endif()
  
  # Iniparser
  if(INIPARSER_FOUND AND INIPARSER_INCLUDE_DIRS)
    target_include_directories(${tgt} PUBLIC ${INIPARSER_INCLUDE_DIRS})
    message(STATUS "Patch: Added Iniparser includes to target ${tgt}")
  endif()
  
  # FFTW3f
  if(HAVE_FFTW3F AND (FFTW3F_INCLUDE_DIRS OR FFTW3F_INCLUDEDIR))
    set(_FFTW_INCS ${FFTW3F_INCLUDE_DIRS} ${FFTW3F_INCLUDEDIR})
    list(REMOVE_DUPLICATES _FFTW_INCS)
    target_include_directories(${tgt} PUBLIC ${_FFTW_INCS})
    message(STATUS "Patch: Added FFTW3f includes to target ${tgt}")
  endif()
endfunction()

# ==================== FINAL DEPENDENCY CHECK ====================
# Report all missing dependencies and fail if any required ones are missing

if(KR_MISSING_DEPS)
  list(LENGTH KR_MISSING_DEPS _num_missing)
  message("")
  message("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
  message("❌ MISSING REQUIRED DEPENDENCIES (${_num_missing})")
  message("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
  message("")
  foreach(_dep IN LISTS KR_MISSING_DEPS)
    message("  ✗ ${_dep}")
  endforeach()
  message("")
  message("Please install the missing dependencies listed above.")
  message("See the WARNING messages earlier in this output for install commands.")
  message("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
  message("")
  message(FATAL_ERROR "Configuration failed due to missing required dependencies.")
endif()

if(KR_MISSING_OPTIONAL_DEPS)
  list(LENGTH KR_MISSING_OPTIONAL_DEPS _num_optional)
  message("")
  message("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
  message("⚠️  MISSING OPTIONAL DEPENDENCIES (${_num_optional})")
  message("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
  message("")
  foreach(_dep IN LISTS KR_MISSING_OPTIONAL_DEPS)
    message("  ⚠️  ${_dep}")
  endforeach()
  message("")
  message("Build will continue, but some features may be unavailable.")
  message("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
  message("")
endif()

# Success message if everything is found
if(NOT KR_MISSING_DEPS AND NOT KR_MISSING_OPTIONAL_DEPS)
  message("")
  message("✓ All dependencies found successfully!")
  message("")
endif()
