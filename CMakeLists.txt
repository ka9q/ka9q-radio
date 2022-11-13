cmake_minimum_required(VERSION 3.10)

project(ka9q-radio)

set(UDEV_RULES_PATH
    "/etc/udev/rules.d"
    CACHE STRING
    "Target directory for udev rule installation."
)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED True)

add_compile_options(-march=native -Wall)
add_compile_definitions(_GNU_SOURCE=1 NDEBUG=1)

if("${CMAKE_BUILD_TYPE}" STREQUAL "Debug")
add_compile_options(-O0)
elseif("${CMAKE_BUILD_TYPE}" STREQUAL "RelWithDebInfo")
else()
add_compile_options(-O3)
endif()

include(GNUInstallDirs)
set(VARDIR "${CMAKE_INSTALL_PREFIX}${CMAKE_INSTALL_LOCALSTATEDIR}/${CMAKE_PROJECT_NAME}")

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
link_libraries(Threads::Threads)

find_library(FFTW3_THREADS_LIB NAMES fftw3f_threads REQUIRED)

find_library(INIPARSER_LIB NAMES iniparser PATHS /opt/local/include/iniparser/ REQUIRED)
find_path(INIPARSER_INCLUDE_DIR iniparser.h PATH_SUFFIXES iniparser)

find_library(PIGPIO_LIB NAMES pigpio)
find_path(PIGPIO_INCLUDE_DIR pigpio.h)

find_library(SDRPLAY_API_LIB NAMES sdrplay_api)
find_path(SDRPLAY_API_H_PATH sdrplay_api.h)

find_package(PkgConfig REQUIRED)
pkg_check_modules(AVAHI avahi-client REQUIRED IMPORTED_TARGET)
pkg_check_modules(FFTW3 fftw3f REQUIRED IMPORTED_TARGET)
pkg_check_modules(LIBAIRSPY libairspy IMPORTED_TARGET)
pkg_check_modules(LIBAIRSPYHF libairspyhf IMPORTED_TARGET)
if(CMAKE_SYSTEM_NAME MATCHES "Linux")
pkg_check_modules(LIBBSD libbsd REQUIRED)
endif()
pkg_check_modules(LIBRTLSDR librtlsdr)
pkg_check_modules(LIBUSB libusb-1.0 IMPORTED_TARGET)
pkg_check_modules(OPUS opus IMPORTED_TARGET)
pkg_check_modules(PORTAUDIO portaudio-2.0 REQUIRED IMPORTED_TARGET)

include_directories(${FFTW3_INCLUDE_DIRS} ${INIPARSER_INCLUDE_DIR})

pkg_check_modules(SYSTEMD "systemd")
if(SYSTEMD_FOUND AND "${SYSTEMD_UNITDIR}" STREQUAL "")
    execute_process(
      COMMAND ${PKG_CONFIG_EXECUTABLE} --variable=systemdsystemunitdir systemd
      OUTPUT_VARIABLE SYSTEMD_UNITDIR
    )
    string(REGEX REPLACE "[ \t\n]+" "" SYSTEMD_UNITDIR "${SYSTEMD_UNITDIR}")
endif()

set(CURSES_NEED_WIDE TRUE)
set(CURSES_NEED_NCURSES TRUE)
find_package(Curses)

add_library(daemons_common OBJECT avahi.c config.c misc.c multicast.c status.c)
target_include_directories(daemons_common PUBLIC ${AVAHI_INCLUDE_DIRS} ${LIBBSD_INCLUDE_DIRS})
set(DAEMONS_COMMON_LIBS PkgConfig::AVAHI ${INIPARSER_LIB} ${LIBBSD_LDFLAGS} m)

## Daemons

if(LIBAIRSPY_FOUND)
    add_executable(airspyd airspyd.c $<TARGET_OBJECTS:daemons_common>)
    target_include_directories(airspyd PUBLIC ${LIBAIRSPY_INCLUDE_DIRS})
    target_link_libraries(airspyd ${DAEMONS_COMMON_LIBS} PkgConfig::LIBAIRSPY)
    install(TARGETS airspyd RUNTIME DESTINATION ${CMAKE_INSTALL_SBINDIR})
endif()

if(LIBAIRSPYHF_FOUND)
    add_executable(airspyhfd airspyhfd.c $<TARGET_OBJECTS:daemons_common>)
    target_include_directories(airspyhfd PUBLIC ${LIBAIRSPYHF_INCLUDE_DIRS})
    target_link_libraries(airspyhfd ${DAEMONS_COMMON_LIBS} PkgConfig::LIBAIRSPYHF)
    install(TARGETS airspyhfd RUNTIME DESTINATION ${CMAKE_INSTALL_SBINDIR})
endif()

add_executable(aprs aprs.c ax25.c $<TARGET_OBJECTS:daemons_common>)
target_include_directories(aprs PUBLIC)
target_link_libraries(aprs ${DAEMONS_COMMON_LIBS})

add_executable(aprsfeed aprsfeed.c ax25.c $<TARGET_OBJECTS:daemons_common>)
target_include_directories(aprsfeed PUBLIC)
target_link_libraries(aprsfeed ${DAEMONS_COMMON_LIBS})

add_executable(cwd cwd.c morse.c osc.c $<TARGET_OBJECTS:daemons_common>)
target_include_directories(cwd PUBLIC)
target_link_libraries(cwd ${DAEMONS_COMMON_LIBS})

if(LIBUSB_FOUND)
    include(FindIconv)
    add_executable(funcubed fcd.c funcubed.c hid-libusb.c $<TARGET_OBJECTS:daemons_common>)
    target_include_directories(funcubed PUBLIC ${Iconv_INCLUDE_DIRS} ${LIBUSB_INCLUDE_DIRS} ${PORTAUDIO_INCLUDE_DIRS} )
    target_link_libraries(funcubed ${DAEMONS_COMMON_LIBS} ${Iconv_LIBRARIES} PkgConfig::LIBUSB PkgConfig::PORTAUDIO)
endif()

if(OPUS_FOUND)
    add_executable(opusd opusd.c $<TARGET_OBJECTS:daemons_common>)
    target_include_directories(opusd PUBLIC ${OPUS_INCLUDE_DIRS})
    target_link_libraries(opusd ${DAEMONS_COMMON_LIBS} PkgConfig::OPUS)
endif()

add_executable(packetd ax25.c filter.c osc.c packetd.c $<TARGET_OBJECTS:daemons_common>)
target_include_directories(packetd PUBLIC ${FFTW3_INCLUDE_DIRS})
target_link_libraries(packetd ${DAEMONS_COMMON_LIBS} ${FFTW3_THREADS_LIB} PkgConfig::FFTW3)

add_executable(radiod audio.c decode_status.c filter.c fm.c iir.c linear.c main.c modes.c osc.c radio.c radio_status.c rtcp.c wfm.c $<TARGET_OBJECTS:daemons_common>)
target_include_directories(radiod PUBLIC ${FFTW3_INCLUDE_DIRS})
target_link_libraries(radiod ${DAEMONS_COMMON_LIBS} ${FFTW3_THREADS_LIB} PkgConfig::FFTW3)

add_executable(rdsd filter.c rdsd.c $<TARGET_OBJECTS:daemons_common>)
target_include_directories(rdsd PUBLIC ${FFTW3_INCLUDE_DIRS})
target_link_libraries(rdsd ${DAEMONS_COMMON_LIBS} ${FFTW3_THREADS_LIB} PkgConfig::FFTW3)

if(LIBRTLSDR_FOUND)
    if(NOT APPLE)
        add_executable(rtlsdrd rtlsdrd.c $<TARGET_OBJECTS:daemons_common>)
        target_include_directories(rtlsdrd PUBLIC ${LIBRTLSDR_INCLUDE_DIRS})
        target_link_libraries(rtlsdrd ${DAEMONS_COMMON_LIBS} ${LIBRTLSDR_LDFLAGS})
        install(TARGETS rtlsdrd RUNTIME DESTINATION ${CMAKE_INSTALL_SBINDIR})
    else()
        message(WARNING "rtlsdrd build disabled on Mac OS X due to build issues")
    endif()
endif()

if(SDRPLAY_API_LIB)
    add_executable(sdrplayd sdrplayd.c $<TARGET_OBJECTS:daemons_common>)
    target_include_directories(sdrplayd PUBLIC ${SDRPLAY_API_H_PATH})
    target_link_libraries(sdrplayd ${DAEMONS_COMMON_LIBS} ${SDRPLAY_API_LIB})
    install(TARGETS sdrplayd RUNTIME DESTINATION ${CMAKE_INSTALL_SBINDIR})
endif()

add_executable(stereod filter.c stereod.c $<TARGET_OBJECTS:daemons_common>)
target_link_libraries(stereod ${DAEMONS_COMMON_LIBS} ${FFTW3_THREADS_LIB} PkgConfig::FFTW3)

install(
    TARGETS
        aprs
        aprsfeed
        cwd
        funcubed
        opusd
        packetd
        radiod
        rdsd
        stereod
    RUNTIME
    DESTINATION ${CMAKE_INSTALL_SBINDIR}
)

## CLI utils

add_executable(control bandplan.c config.c control.c decode_status.c misc.c modes.c multicast.c status.c)
target_include_directories(control PUBLIC ${CURSES_INCLUDE_DIRS} ${FFTW3_INCLUDE_DIRS} ${LIBBSD_INCLUDE_DIRS})
target_link_libraries(control ${CURSES_LIBRARIES} ${FFTW3_THREADS_LIB} PkgConfig::FFTW3 ${INIPARSER_LIB} ${LIBBSD_LDFLAGS} m)

add_executable(iqplay attr.c avahi.c iqplay.c misc.c multicast.c status.c)
target_include_directories(iqplay PUBLIC ${AVAHI_INCLUDE_DIRS} ${LIBBSD_INCLUDE_DIRS})
target_link_libraries(iqplay PkgConfig::AVAHI ${LIBBSD_LDFLAGS} m)

add_executable(iqrecord attr.c decode_status.c iqrecord.c multicast.c status.c)
target_include_directories(iqrecord PUBLIC ${FFTW3_INCLUDE_DIRS} ${LIBBSD_INCLUDE_DIRS})
target_link_libraries(iqrecord ${FFTW3_THREADS_LIB} PkgConfig::FFTW3 ${LIBBSD_LDFLAGS} m)

add_executable(metadump dump.c metadump.c misc.c multicast.c status.c)
target_include_directories(metadump PUBLIC ${FFTW3_INCLUDE_DIRS} ${LIBBSD_INCLUDE_DIRS})
target_link_libraries(metadump ${FFTW3_THREADS_LIB} PkgConfig::FFTW3 ${LIBBSD_LDFLAGS} m)

add_executable(modulate filter.c misc.c modulate.c osc.c)
target_include_directories(modulate PUBLIC ${FFTW3_INCLUDE_DIRS})
target_link_libraries(modulate ${FFTW3_THREADS_LIB} PkgConfig::FFTW3 m)

add_executable(monitor config.c iir.c misc.c monitor.c morse.c multicast.c osc.c)
target_include_directories(monitor PUBLIC ${CURSES_INCLUDE_DIRS} ${LIBBSD_INCLUDE_DIRS} ${OPUS_INCLUDE_DIRS} ${PORTAUDIO_INCLUDE_DIRS})
target_link_libraries(monitor ${CURSES_LIBRARIES} ${INIPARSER_LIB} ${LIBBSD_LDFLAGS} PkgConfig::OPUS PkgConfig::PORTAUDIO m)

add_executable(opussend multicast.c opussend.c)
target_include_directories(opussend PUBLIC ${LIBBSD_INCLUDE_DIRS} ${OPUS_INCLUDE_DIRS} ${PORTAUDIO_INCLUDE_DIRS})
target_link_libraries(opussend ${LIBBSD_LDFLAGS} PkgConfig::OPUS PkgConfig::PORTAUDIO m)

add_executable(pcmcat multicast.c pcmcat.c)
target_include_directories(pcmcat PUBLIC ${LIBBSD_INCLUDE_DIRS})
target_link_libraries(pcmcat ${LIBBSD_LDFLAGS})

add_executable(pcmrecord attr.c multicast.c pcmrecord.c)
target_include_directories(pcmrecord PUBLIC ${LIBBSD_INCLUDE_DIRS})
target_link_libraries(pcmrecord ${LIBBSD_LDFLAGS})

add_executable(pcmsend multicast.c pcmsend.c)
target_include_directories(pcmsend PUBLIC ${LIBBSD_INCLUDE_DIRS} ${PORTAUDIO_INCLUDE_DIRS})
target_link_libraries(pcmsend ${LIBBSD_LDFLAGS} PkgConfig::PORTAUDIO)

add_executable(pcmspawn multicast.c pcmspawn.c status.c)
target_include_directories(pcmspawn PUBLIC ${LIBBSD_INCLUDE_DIRS})
target_link_libraries(pcmspawn ${LIBBSD_LDFLAGS})

add_executable(pl filter.c misc.c multicast.c osc.c pl.c)
target_include_directories(pl PUBLIC ${FFTW3_INCLUDE_DIRS} ${LIBBSD_INCLUDE_DIRS})
target_link_libraries(pl ${FFTW3_THREADS_LIB} PkgConfig::FFTW3 ${LIBBSD_LDFLAGS} m)

add_executable(show-pkt multicast.c show-pkt.c status.c)
target_include_directories(show-pkt PUBLIC ${CURSES_INCLUDE_DIRS} ${LIBBSD_INCLUDE_DIRS})
target_link_libraries(show-pkt ${CURSES_LIBRARIES} ${LIBBSD_LDFLAGS})

add_executable(setfilt multicast.c setfilt.c status.c)
target_include_directories(setfilt PUBLIC ${LIBBSD_INCLUDE_DIRS})
target_link_libraries(setfilt ${LIBBSD_LDFLAGS})

if(PIGPIO_INCLUDE_DIR)
    # TODO: Not tested.
    add_executable(set_xcvr config.c set_xcvr.c)
    target_include_directories(set_xcvr PUBLIC ${PIGPIO_INCLUDE_DIR})
    target_link_libraries(set_xcvr ${INIPARSER_LIB} m)
endif()

add_executable(show-sig multicast.c show-sig.c status.c)
target_include_directories(show-sig PUBLIC ${CURSES_INCLUDE_DIRS} ${LIBBSD_INCLUDE_DIRS})
target_link_libraries(show-sig ${CURSES_LIBRARIES} ${LIBBSD_LDFLAGS} m)

add_executable(tune misc.c multicast.c status.c tune.c)
target_include_directories(tune PUBLIC ${LIBBSD_INCLUDE_DIRS})
target_link_libraries(tune ${LIBBSD_LDFLAGS} m)

add_executable(wspr-decoded attr.c multicast.c wspr-decoded.c)
target_include_directories(wspr-decoded PUBLIC ${LIBBSD_INCLUDE_DIRS})
target_link_libraries(wspr-decoded ${LIBBSD_LDFLAGS})

## Other

install(FILES bandplan.txt help.txt modes.conf id.txt monitor-help.txt DESTINATION share/${CMAKE_PROJECT_NAME})
if(${SYSTEMD_UNITDIR})
    install(FILES
        airspyd@.service
        airspyhfd@.service
        aprsfeed.service
        cwd.service
        funcubed@.service
        hackrf@.service
        horusdemod.service
        opusd@.service
        packetd.service
        radiod@.service
        rdsd.service
        recordings@.service
        sdrplayd@.service
        stereod.service
        wspr-decoded.service
        DESTINATION ${SYSTEMD_UNITDIR})
endif()

if(${CMAKE_SYSTEM_NAME} STREQUAL "Linux")
    install(FILES
            20-rtlsdr.rules
            52-airspy.rules
            52-airspyhf.rules
            66-hackrf.rules
            68-funcube-dongle-proplus.rules
            68-funcube-dongle.rules
        DESTINATION ${UDEV_RULES_PATH}
        COMPONENT "udev_rules"
    )

    install(FILES
            airspyd.conf
            aprs.conf
            funcubed.conf
            horus.conf
            opusd@aviation.conf
            opusd@fm.conf
            opusd@hf.conf
            opusd@hfdl.conf
            opusd@kpbs.conf
            opusd@ksds.conf
            opusd@packet.conf
            packetd.conf
            radiod@aviation.conf
            radiod@fm.conf
            radiod@hf.conf
            radiod@horus.conf
            radiod@nws.conf
            radiod@repeater.conf
            radiod@sonde.conf
            radiod@10m.conf
            radiod@125cm.conf
            radiod@2m.conf
            radiod@6m.conf
            radiod@70cm.conf
            rdsd.conf
            recordings@aviation.conf
            recordings@fm.conf
            recordings@hf.conf
            recordings@pictures.conf
            stereod.conf
            wspr-decoded.conf
        DESTINATION "/${CMAKE_INSTALL_SYSCONFDIR}/radio"
    )

    install(FILES aprsfeed.rotate DESTINATION "/${CMAKE_INSTALL_SYSCONFDIR}/logrotate.d")
    install(FILES airspy-blacklist.conf DESTINATION "/${CMAKE_INSTALL_SYSCONFDIR}/modprobe.d")
    install(FILES 98-sockbuf.conf DESTINATION "/${CMAKE_INSTALL_SYSCONFDIR}/sysctl.d")
    install(FILES start-ka9q-horus.sh DESTINATION ${CMAKE_INSTALL_SBINDIR})

    install(CODE "
        set(CMAKE_EXECUTE_PROCESS_COMMAND_ECHO STDOUT)
        execute_process(COMMAND mkdir -p /etc/fftw /etc/radio ${VARDIR} CMDECHO_ERROR_VARIABLE)
        execute_process(COMMAND /usr/sbin/adduser --quiet --system --group radio CMDECHO_ERROR_VARIABLE)
        execute_process(COMMAND /usr/sbin/adduser --quiet --system --ingroup radio airspy CMDECHO_ERROR_VARIABLE)
        execute_process(COMMAND /usr/sbin/adduser --quiet --system --ingroup radio funcube CMDECHO_ERROR_VARIABLE)
        execute_process(COMMAND /usr/sbin/adduser --quiet --system --ingroup radio recordings CMDECHO_ERROR_VARIABLE)
        execute_process(COMMAND /usr/sbin/adduser --quiet --system --ingroup radio aprsfeed CMDECHO_ERROR_VARIABLE)
        execute_process(COMMAND chgrp radio /etc/fftw /etc/radio ${VARDIR} CMDECHO_ERROR_VARIABLE)
        execute_process(COMMAND chmod g+ws /etc/fftw /etc/radio ${VARDIR} CMDECHO_ERROR_VARIABLE)
        execute_process(COMMAND systemctl daemon-reload CMDECHO_ERROR_VARIABLE)
        "
    )
endif()
