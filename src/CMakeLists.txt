# === OPTIONS: Optional features toggled from top-level CMakeLists ===
option(ENABLE_XATTR "Build xattr helpers (attr.c)" ON)
option(ENABLE_SDR_DRIVERS "Build SDR driver modules" ON)

# === SOURCES: Core library source files ===
set(LIBRADIO_SOURCES
  ax25.c morse.c bandplan.c dump.c modes.c
  avahi.c avahi_browse.c
  decode_status.c status.c misc.c multicast.c rtp.c osc.c config.c
  filter.c iir.c
)

if(ENABLE_XATTR)
  list(APPEND LIBRADIO_SOURCES attr.c)
endif()

# === LIBRARY: Build the main radio static library ===
add_library(radio STATIC ${LIBRADIO_SOURCES})
target_include_directories(radio
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_BINARY_DIR}/generated
)

# === LINK: Common dependencies defined in FindDeps.cmake ===
kr_link_common_deps(radio)

# === DEP: Link iniparser if found ===
if(INIPARSER_FOUND)
  target_link_libraries(radio PUBLIC iniparser::iniparser)
else()
  message(FATAL_ERROR "iniparser not found. e.g., macOS: brew install iniparser; Debian/Ubuntu: apt install libiniparser-dev")
endif()

# === DEP: Link opus if found ===
if(OPUS_FOUND)
  target_link_libraries(radio PUBLIC opus::opus)
else()
  message(FATAL_ERROR "opus not found. e.g., macOS: brew install opus; Debian/Ubuntu: apt install libopus-dev")
endif()

# === FUNCTION: Helper to define tools that link against libradio ===
function(add_tool name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE radio)
  install(TARGETS ${name} RUNTIME DESTINATION bin)
endfunction()

# === TOOLS: Build small tools ===
add_tool(packetd   packetd.c)
add_tool(fft-gen   fft-gen.c)
add_tool(pl        pl.c)
add_tool(powers    powers.c)
add_tool(tune      tune.c)
add_tool(control   control.c)
add_tool(show-sig  show-sig.c)
add_tool(show-pkt  show-pkt.c)
add_tool(metadump  metadump.c)
add_tool(setfilt   setfilt.c)
add_tool(cwd       cwd.c)

# === RADIOD: Collect sources, enable HID only if libusb is found ===
set(RADIOD_SRCS
  main.c audio.c fm.c wfm.c linear.c spectrum.c
  radio.c radio_status.c rtcp.c fcd.c
)

# Optional: Add HID support only if libusb is available
if(TARGET LibUSB::libusb-1.0)
  list(APPEND RADIOD_SRCS hid-libusb.c)
endif()

# === HIDAPI: Enforce requirement if necessary ===
if(HIDAPI_REQUIRED_FOR_RADIOD AND NOT HIDAPI_FOUND)
  message(FATAL_ERROR "HIDAPI is required for building 'radiod', but not found.")
endif()

# === TARGET: Build radiod executable ===
add_executable(radiod ${RADIOD_SRCS})

# === INCLUDE PATHS: radiod-specific ===
target_include_directories(radiod PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}        # for compat_libusb.h
  ${LIBUSB_INCLUDE_DIRS}             # may be empty on FreeBSD
)

# === LINK: Link libraries for radiod ===
target_link_libraries(radiod
  PRIVATE
    radio
    opus::opus
    iniparser::iniparser
)

# HIDAPI linking (only if available)
if(HIDAPI_FOUND)
  target_link_libraries(radiod PRIVATE ${HIDAPI_LIBRARIES})
endif()

# libusb linking and feature flags
if(TARGET LibUSB::libusb-1.0)
  target_link_libraries(radiod PRIVATE LibUSB::libusb-1.0)
  target_compile_definitions(radiod PRIVATE HAVE_LIBUSB_1_0=1)
else()
  message(STATUS "libusb not found: building radiod without HID support")
endif()

# Pass C macro to reflect availability of libusb_get_string_descriptor
if(DEFINED HAVE_LIBUSB_GET_STRING_DESCRIPTOR)
  target_compile_definitions(radiod PRIVATE
    HAVE_LIBUSB_GET_STRING_DESCRIPTOR=${HAVE_LIBUSB_GET_STRING_DESCRIPTOR})
endif()

# iconv (used in hid-libusb.c on FreeBSD/macOS)
if(DEFINED Iconv_IS_BUILT_IN AND Iconv_IS_BUILT_IN)
  target_compile_definitions(radiod PRIVATE LIBICONV_PLUG=1)
elseif(ICONV_FOUND)
  target_link_libraries(radiod PRIVATE ${ICONV_LIBRARIES})
  if(ICONV_INCLUDE_DIR)
    target_include_directories(radiod PRIVATE ${ICONV_INCLUDE_DIR})
  endif()
else()
  message(WARNING "iconv not found: you may need to install libiconv")
endif()

# === INSTALL: radiod goes to sbin ===
install(TARGETS radiod RUNTIME DESTINATION sbin)

# === DRIVER MODULES: Build only if drivers and deps are enabled ===
function(kr_add_driver name)
  add_library(${name}_drv MODULE ${ARGN})
  target_link_libraries(${name}_drv PRIVATE radio)
  set_target_properties(${name}_drv PROPERTIES
    PREFIX ""              # libfoo.so -> foo.so
    OUTPUT_NAME "${name}"
  )
  install(TARGETS ${name}_drv LIBRARY DESTINATION lib/ka9q-radio)
endfunction()

if(ENABLE_SDR_DRIVERS)
  if(TARGET Airspy::airspy)
    kr_add_driver(airspy airspy.c)
    target_link_libraries(airspy_drv PRIVATE Airspy::airspy)
  endif()
  if(TARGET Airspy::airspyhf)
    kr_add_driver(airspyhf airspyhf.c)
    target_link_libraries(airspyhf_drv PRIVATE Airspy::airspyhf)
  endif()
  if(TARGET RTLSDR::rtlsdr)
    kr_add_driver(rtlsdr rtlsdr.c)
    target_link_libraries(rtlsdr_drv PRIVATE RTLSDR::rtlsdr)
  endif()
  if(TARGET HackRF::hackrf)
    kr_add_driver(hackrf hackrf.c)
    target_link_libraries(hackrf_drv PRIVATE HackRF::hackrf)
  endif()

  # Funcube and RX888 require libusb and HID helpers
  if(TARGET LibUSB::libusb-1.0)
    kr_add_driver(funcube funcube.c fcd.c hid-libusb.c)
    target_link_libraries(funcube_drv PRIVATE LibUSB::libusb-1.0)

    kr_add_driver(rx888 rx888.c ezusb.c hid-libusb.c)
    target_link_libraries(rx888_drv PRIVATE LibUSB::libusb-1.0)
  endif()
endif()

# === DEBUG: Print HIDAPI configuration ===
message(STATUS "HIDAPI lib: ${HIDAPI_LIBRARIES}")
message(STATUS "HIDAPI include: ${HIDAPI_INCLUDE_DIRS}")

