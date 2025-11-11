# src/CMakeLists.txt
# Build configuration for ka9q-radio source files

# ==================== CORE LIBRARY SOURCES ====================
set(LIBRADIO_SOURCES
  ax25.c
  morse.c
  bandplan.c
  dump.c
  modes.c
  avahi.c
  avahi_browse.c
  decode_status.c
  status.c
  misc.c
  multicast.c
  rtp.c
  osc.c
  config.c
  filter.c
  iir.c
)

# Conditionally add xattr support
if(ENABLE_XATTR)
  list(APPEND LIBRADIO_SOURCES attr.c)
  message(STATUS "Building with xattr support")
endif()

# ==================== BUILD CORE LIBRARY ====================
add_library(radio STATIC ${LIBRADIO_SOURCES})

# Set include directories for the library
target_include_directories(radio
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated>
    $<INSTALL_INTERFACE:include/ka9q-radio>
)

# Link compiler flags and common dependencies
target_link_libraries(radio
  PUBLIC
    ka9q::compile_flags
    opus::opus
    iniparser::iniparser
)

# Link common dependencies (FFTW, threads, audio, etc.)
kr_link_common_deps(radio)

# ==================== BSD FUNCTIONS ====================
# Check for strlcpy (BSD function, needs libbsd on Linux)
include(CheckSymbolExists)
check_symbol_exists(strlcpy "string.h" HAVE_STRLCPY)

if(NOT HAVE_STRLCPY)
  if(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
    message(STATUS "strlcpy assumed present in FreeBSD libc")
  else()
    # Need libbsd on Linux
    find_library(LIBBSD_LIBRARY bsd)
    if(NOT LIBBSD_LIBRARY)
      message(FATAL_ERROR "libbsd not found (required for BSD functions like strlcpy on Linux).\n  Install with: sudo apt install libbsd-dev")
    endif()
    target_link_libraries(radio PRIVATE ${LIBBSD_LIBRARY})
    message(STATUS "Linking libbsd for BSD functions")
  endif()
endif()

# ==================== TOOL HELPER FUNCTIONS ====================
# Helper to create small tools that link against libradio
function(add_tool name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE radio)
  install(TARGETS ${name} RUNTIME DESTINATION bin)
endfunction()

# Helper to create daemon programs (installed to sbin)
function(add_daemon name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE radio)
  install(TARGETS ${name} RUNTIME DESTINATION sbin)
endfunction()

# ==================== BUILD SIMPLE TOOLS ====================
# Tools that only need libradio
add_tool(control   control.c)
add_tool(cwd       cwd.c)
add_tool(fft-gen   fft-gen.c)
add_tool(jt-decoded jt-decoded.c)
add_tool(metadump  metadump.c)
add_tool(pl        pl.c)
add_tool(powers    powers.c)
add_tool(setfilt   setfilt.c)
add_tool(show-pkt  show-pkt.c)
add_tool(show-sig  show-sig.c)
add_tool(tune      tune.c)
add_tool(wd-record wd-record.c)

# ==================== BUILD SIMPLE DAEMONS ====================
# Daemons that only need libradio
add_daemon(aprs      aprs.c)
add_daemon(aprsfeed  aprsfeed.c)
add_daemon(opusd     opusd.c)
add_daemon(packetd   packetd.c)

# ==================== BUILD MONITOR ====================
# Monitor is a multi-file program with extra dependencies
if(TARGET Ncurses::w AND TARGET PortAudio::portaudio AND TARGET SampleRate::samplerate)
  add_tool(monitor 
    monitor.c 
    monitor-data.c 
    monitor-display.c 
    monitor-repeater.c
  )
  
  # Monitor needs ncursesw, portaudio, samplerate beyond what radio provides
  target_link_libraries(monitor PRIVATE 
    Ncurses::w
    PortAudio::portaudio
    SampleRate::samplerate
  )
  
  # Linux also needs ALSA
  if(TARGET ALSA::alsa)
    target_link_libraries(monitor PRIVATE ALSA::alsa)
  endif()
  
  message(STATUS "Building monitor")
else()
  message(STATUS "Skipping monitor - requires ncursesw, portaudio, and libsamplerate")
endif()

# ==================== BUILD PCMRECORD ====================
# Audio recording tool
if(TARGET Ogg::ogg)
  add_tool(pcmrecord pcmrecord.c)
  target_link_libraries(pcmrecord PRIVATE Ogg::ogg)
  message(STATUS "Building pcmrecord")
else()
  message(STATUS "Skipping pcmrecord - requires libogg")
endif()

# ==================== BUILD FFTW-BASED DAEMONS ====================
# These need FFTW with threading support
if(TARGET FFTW::fftw3f AND TARGET FFTW::fftw3f_threads)
  add_daemon(stereod stereod.c)
  target_link_libraries(stereod PRIVATE FFTW::fftw3f FFTW::fftw3f_threads)
  message(STATUS "Building stereod")
  
  add_daemon(rdsd rdsd.c)
  target_link_libraries(rdsd PRIVATE FFTW::fftw3f FFTW::fftw3f_threads)
  message(STATUS "Building rdsd")
else()
  message(STATUS "Skipping stereod and rdsd - require FFTW with threading")
endif()

# ==================== RADIOD SOURCES ====================
set(RADIOD_SRCS
  main.c
  audio.c
  fm.c
  wfm.c
  linear.c
  spectrum.c
  radio.c
  radio_status.c
  rtcp.c
  fcd.c
)

# Add HID support if libusb is available
if(TARGET LibUSB::libusb-1.0)
  list(APPEND RADIOD_SRCS hid-libusb.c)
  message(STATUS "Building radiod with HID support")
else()
  message(STATUS "Building radiod without HID support (libusb not found)")
endif()

# ==================== BUILD RADIOD ====================
add_executable(radiod ${RADIOD_SRCS})

# Include directories for radiod
target_include_directories(radiod PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}/generated
)

# Link libraries for radiod
# Note: opus and iniparser come transitively through radio (PUBLIC links)
target_link_libraries(radiod PRIVATE
  radio
  m
)

# Enable dynamic symbol export on Linux (needed for dlopen'd modules)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_options(radiod PRIVATE -rdynamic)
endif()

# ==================== LIBUSB CONFIGURATION ====================
if(TARGET LibUSB::libusb-1.0)
  target_link_libraries(radiod PRIVATE LibUSB::libusb-1.0)
  target_compile_definitions(radiod PRIVATE HAVE_LIBUSB_1_0=1)
  
  # Pass feature flag for libusb_get_string_descriptor
  if(DEFINED HAVE_LIBUSB_GET_STRING_DESCRIPTOR)
    target_compile_definitions(radiod PRIVATE
      HAVE_LIBUSB_GET_STRING_DESCRIPTOR=${HAVE_LIBUSB_GET_STRING_DESCRIPTOR})
  endif()
endif()

# ==================== HIDAPI CONFIGURATION ====================
if(HIDAPI_FOUND)
  target_link_libraries(radiod PRIVATE hidapi::hidapi)
  message(STATUS "Linking radiod with HIDAPI")
endif()

# ==================== ICONV CONFIGURATION ====================
# macOS/FreeBSD may need explicit iconv handling
if(DEFINED Iconv_IS_BUILT_IN AND Iconv_IS_BUILT_IN)
  target_compile_definitions(radiod PRIVATE LIBICONV_PLUG=1)
elseif(ICONV_FOUND AND ICONV_LIBRARIES)
  target_link_libraries(radiod PRIVATE ${ICONV_LIBRARIES})
  if(ICONV_INCLUDE_DIR)
    target_include_directories(radiod PRIVATE ${ICONV_INCLUDE_DIR})
  endif()
  message(STATUS "Linking radiod with external iconv")
endif()

# ==================== INSTALL RADIOD ====================
install(TARGETS radiod RUNTIME DESTINATION sbin)

# ==================== SDR DRIVER MODULES ====================
# Drivers are built as loadable modules (plugins)
# They get their symbols from the radiod executable at runtime

# Helper function for driver modules
function(kr_add_driver name)
  add_library(${name}_drv MODULE ${ARGN})
  
  # Drivers need access to headers for compilation
  # Start with source and generated directories
  target_include_directories(${name}_drv PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_BINARY_DIR}/generated
  )
  
  # Add include directories from dependencies (needed for headers)
  # Get them by querying the targets directly
  if(TARGET opus::opus)
    get_target_property(_opus_incs opus::opus INTERFACE_INCLUDE_DIRECTORIES)
    if(_opus_incs)
      target_include_directories(${name}_drv PRIVATE ${_opus_incs})
    endif()
  endif()
  
  if(TARGET iniparser::iniparser)
    get_target_property(_ini_incs iniparser::iniparser INTERFACE_INCLUDE_DIRECTORIES)
    if(_ini_incs)
      target_include_directories(${name}_drv PRIVATE ${_ini_incs})
    endif()
  endif()
  
  if(TARGET FFTW::fftw3f)
    get_target_property(_fftw_incs FFTW::fftw3f INTERFACE_INCLUDE_DIRECTORIES)
    if(_fftw_incs)
      target_include_directories(${name}_drv PRIVATE ${_fftw_incs})
    endif()
  endif()
  
  # Set properties for plugin naming
  set_target_properties(${name}_drv PROPERTIES
    PREFIX ""              # Remove 'lib' prefix
    OUTPUT_NAME "${name}"  # Output as 'airspy.so' not 'airspy_drv.so'
  )
  
  # Platform-specific linking for dynamic modules
  if(APPLE)
    # macOS: Allow undefined symbols (resolved at runtime by radiod)
    target_link_options(${name}_drv PRIVATE
      -bundle
      -Wl,-undefined,dynamic_lookup
    )
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Linux: Shared library (radiod exports symbols with -rdynamic)
    # No special flags needed here
  elseif(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
    # FreeBSD: Similar to Linux
    # No special flags needed
  endif()
  
  install(TARGETS ${name}_drv LIBRARY DESTINATION lib/ka9q-radio)
endfunction()

# Helper to check if driver headers are accessible
function(kr_can_build_driver driver_name target_name result_var)
  # Try to compile a test program with the driver headers
  include(CheckCSourceCompiles)
  
  # Get include directories from the target
  if(NOT TARGET ${target_name})
    set(${result_var} FALSE PARENT_SCOPE)
    return()
  endif()
  
  get_target_property(_includes ${target_name} INTERFACE_INCLUDE_DIRECTORIES)
  
  if(_includes)
    set(_saved_includes ${CMAKE_REQUIRED_INCLUDES})
    set(CMAKE_REQUIRED_INCLUDES ${_includes})
    
    # Driver-specific header tests
    if(driver_name STREQUAL "airspy")
      check_c_source_compiles("
        #include <libairspy/airspy.h>
        int main(void) { return 0; }
      " CAN_BUILD_${driver_name})
    elseif(driver_name STREQUAL "airspyhf")
      check_c_source_compiles("
        #include <libairspyhf/airspyhf.h>
        int main(void) { return 0; }
      " CAN_BUILD_${driver_name})
    elseif(driver_name STREQUAL "rtlsdr")
      check_c_source_compiles("
        #include <rtl-sdr.h>
        int main(void) { return 0; }
      " CAN_BUILD_${driver_name})
    elseif(driver_name STREQUAL "hackrf")
      check_c_source_compiles("
        #include <libhackrf/hackrf.h>
        int main(void) { return 0; }
      " CAN_BUILD_${driver_name})
    else()
      # Unknown driver, assume buildable
      set(CAN_BUILD_${driver_name} TRUE)
    endif()
    
    set(CMAKE_REQUIRED_INCLUDES ${_saved_includes})
    set(${result_var} ${CAN_BUILD_${driver_name}} PARENT_SCOPE)
  else()
    # No includes property, assume buildable
    set(${result_var} TRUE PARENT_SCOPE)
  endif()
endfunction()

# Build drivers if enabled
if(ENABLE_SDR_DRIVERS)
  message(STATUS "Building SDR driver modules...")
  
  # Airspy R2/Mini
  if(TARGET Airspy::airspy)
    kr_can_build_driver("airspy" "Airspy::airspy" CAN_BUILD_AIRSPY)
    if(CAN_BUILD_AIRSPY)
      kr_add_driver(airspy airspy.c)
      target_link_libraries(airspy_drv PRIVATE Airspy::airspy)
      message(STATUS "  ✓ Building airspy driver")
    else()
      message(WARNING "  ✗ Skipping airspy driver - headers not accessible")
      message(STATUS "    Fix: brew reinstall libairspy")
    endif()
  endif()
  
  # Airspy HF+
  if(TARGET Airspy::airspyhf)
    kr_can_build_driver("airspyhf" "Airspy::airspyhf" CAN_BUILD_AIRSPYHF)
    if(CAN_BUILD_AIRSPYHF)
      kr_add_driver(airspyhf airspyhf.c)
      target_link_libraries(airspyhf_drv PRIVATE Airspy::airspyhf)
      message(STATUS "  ✓ Building airspyhf driver")
    else()
      message(WARNING "  ✗ Skipping airspyhf driver - headers not accessible")
      message(STATUS "    Fix: brew reinstall libairspyhf")
    endif()
  endif()
  
  # RTL-SDR
  if(TARGET RTLSDR::rtlsdr)
    kr_can_build_driver("rtlsdr" "RTLSDR::rtlsdr" CAN_BUILD_RTLSDR)
    if(CAN_BUILD_RTLSDR)
      kr_add_driver(rtlsdr rtlsdr.c)
      target_link_libraries(rtlsdr_drv PRIVATE RTLSDR::rtlsdr)
      message(STATUS "  ✓ Building rtlsdr driver")
    else()
      message(WARNING "  ✗ Skipping rtlsdr driver - headers not accessible")
      message(STATUS "    Fix: brew reinstall librtlsdr")
    endif()
  endif()
  
  # HackRF
  if(TARGET HackRF::hackrf)
    kr_can_build_driver("hackrf" "HackRF::hackrf" CAN_BUILD_HACKRF)
    if(CAN_BUILD_HACKRF)
      kr_add_driver(hackrf hackrf.c)
      target_link_libraries(hackrf_drv PRIVATE HackRF::hackrf)
      # HackRF also needs libusb on some platforms
      if(TARGET LibUSB::libusb-1.0)
        target_link_libraries(hackrf_drv PRIVATE LibUSB::libusb-1.0)
      endif()
      message(STATUS "  ✓ Building hackrf driver")
    else()
      message(WARNING "  ✗ Skipping hackrf driver - headers not accessible")
      message(STATUS "    Fix: brew reinstall hackrf")
    endif()
  endif()
  
  # Funcube and RX888 require libusb for HID support
  # These don't have header path issues
  if(TARGET LibUSB::libusb-1.0)
    kr_add_driver(funcube funcube.c fcd.c hid-libusb.c)
    target_link_libraries(funcube_drv PRIVATE LibUSB::libusb-1.0)
    # Funcube also uses PortAudio
    if(TARGET PortAudio::portaudio)
      target_link_libraries(funcube_drv PRIVATE PortAudio::portaudio)
    endif()
    message(STATUS "  ✓ Building funcube driver")
    
    kr_add_driver(rx888 rx888.c ezusb.c hid-libusb.c)
    target_link_libraries(rx888_drv PRIVATE LibUSB::libusb-1.0)
    message(STATUS "  ✓ Building rx888 driver")
  else()
    message(STATUS "  ✗ Skipping funcube and rx888 drivers (require libusb)")
  endif()
else()
  message(STATUS "SDR driver modules disabled (ENABLE_SDR_DRIVERS=OFF)")
endif()
