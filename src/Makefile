# -*- makefile -*-

# Primary Linux makefile for ka9q-radio package
# Copyright 2017-2025, Phil Karn, KA9Q
# By default, builds release version
# To build for debugging say "make BUILD=debug"

BUILD ?= release

UNAME_S := $(shell uname -s)

CPPFLAGS ?=
LDFLAGS  ?=

LDLIBS = -lm -lpthread

ifeq ($(UNAME_S),Darwin)
  CPPFLAGS += -I/opt/local/include
  LDFLAGS  += -L/opt/local/lib
  LDLIBS += -lintl -liconv
  SOFLAGS = -bundle -Wl,-undefined,dynamic_lookup
else
  LDLIBS += -lbsd
  SOFLAGS = -shared
endif

ifeq ($(BUILD),debug)
     DOPTS = -g
else
     DOPTS = -DNDEBUG=1 -O3
endif

ifdef SANITIZE
     DOPTS += -fsanitize=address -fsanitize=undefined
     LDOPTS = -fsanitize=address -fsanitize=undefined
endif

ARCHOPTS = -march=native
# do NOT set -ffast-math or -ffinite-math-only; NANs are widely used as 'variable not set' sentinels
COPTS = -std=gnu11 -pthread -Wall -Wno-gnu-folding-constant -funsafe-math-optimizations -fno-math-errno -fcx-limited-range -freciprocal-math -fno-trapping-math -D_GNU_SOURCE=1 -Wextra -MMD -MP
COPTS += -fPIC
CFLAGS = $(DOPTS) $(ARCHOPTS) $(COPTS) $(INCLUDES)
BINDIR = /usr/local/bin
SODIR = /usr/local/lib/ka9q-radio
DAEMONDIR = /usr/local/sbin
DAEMONS = aprs aprsfeed cwd packetd radiod
EXECS = control fft-gen jt-decoded metadump monitor pcmrecord pl powers tune wd-record
LIBRADIO = ax25.o morse.o bandplan.o dump.o modes.o avahi.o avahi_browse.o attr.o filter.o iir.o decode_status.o status.o misc.o multicast.o rtp.o osc.o config.o
DYNAMIC_DRIVERS = rx888.so airspyhf.so airspy.so sig_gen.so funcube.so rtlsdr.so
RADIOD_OBJECTS = main.o audio.o fm.o wfm.o linear.o spectrum.o radio.o radio_status.o rtcp.o fcd.o hid-libusb.o libradio.a
CFILES = airspy.c airspyhf.c aprs.c aprsfeed.c attr.c audio.c avahi.c avahi_browse.c ax25.c bandplan.c config.c control.c cwd.c decimate.c decode_status.c dump.c ezusb.c fcd.c filter.c fm.c funcube.c hid-libusb.c iir.c jt-decoded.c linear.c main.c metadump.c misc.c modes.c monitor.c monitor-data.c monitor-display.c monitor-repeater.c morse.c multicast.c opusd.c opussend.c osc.c packetd.c pcmcat.c pcmrecord.c pcmsend.c pcmspawn.c pl.c powers.c radio.c radio_status.c rdsd.c rtcp.c rtlsdr.c rtp.c rx888.c setfilt.c show-pkt.c show-sig.c sig_gen.c spectrum.c status.c stereod.c tune.c wd-record.c wfm.c
HFILES = attr.h ax25.h bandplan.h conf.h config.h decimate.h ezusb.h fcd.h fcdhidcmd.h filter.h hidapi.h iir.h misc.h monitor.h morse.h multicast.h osc.h radio.h rx888.h status.h

# Build HackRF support if installed, or if 'make HACKRF=1'
ifeq ($(wildcard /usr/include/libhackrf/hackrf.h),)
else
   HACKRF=1
endif
# if HACKRF is set, build it
ifdef HACKRF
   CFILES += hackrf.c
   DYNAMIC_DRIVERS += hackrf.so
endif

# Build SDRPLAY support if installed, or if 'make SDRPLAY=1'
ifeq ($(wildcard /usr/local/include/sdrplay_api.h),)
else
   SDRPLAY=1
endif
# If SDRPLAY is set, add the necessary flags, files, and libraries
ifdef SDRPLAY
   CFILES += sdrplay.c
   DYNAMIC_DRIVERS += sdrplay.so
endif

# Build FOBOS support if installed, or if 'make FOBOS=1'
ifeq ($(wildcard /usr/local/include/fobos.h /usr/include/fobos.h),)
else
   FOBOS=1
endif
# if FOBOS is set, build it too
ifdef FOBOS
   CFILES += fobos.c
   DYNAMIC_DRIVERS += fobos.so
endif

# Build if installed
ifeq ($(wildcard /usr/local/include/libbladeRF.h /usr/include/libbladeRF.h),)
else
   BLADERF=1
endif
# if BLADERF is set, build it too
ifdef BLADERF
   CFILES += bladerf.c
   DYNAMIC_DRIVERS += bladerf.so
endif

# Build HydraSDR support if installed, or if 'make HYDRASDR=1'
ifeq ($(wildcard /usr/local/include/libhydrasdr/hydrasdr.h /usr/include/libhydrasdr/hydrasdr.h),)
else
   HYDRASDR=1
endif
# if HYDRASDR is set, build it too
ifdef HYDRASDR
   CFILES += hydrasdr.c
   DYNAMIC_DRIVERS += hydrasdr.so
endif

# Build on raspberry pi only
ifeq ($(wildcard /usr/include/pigpio.h),)
else
   EXECS += set_xcvr
endif

all: $(DAEMONS) $(EXECS) $(DYNAMIC_DRIVERS)

install: install-$(UNAME_S)

install-Linux: $(DAEMONS) $(EXECS) $(DYNAMIC_DRIVERS)
	@echo "Installing for Linux..."
	rsync -a $(DYNAMIC_DRIVERS) $(SODIR)
	ln -sf $(SODIR)/*.so /usr/local/lib
	rsync -a $(DAEMONS) $(DAEMONDIR)
	rsync -a $(EXECS) $(BINDIR)
	setcap cap_sys_nice=ep $(BINDIR)/monitor
	setcap cap_net_admin,cap_sys_nice=ep $(DAEMONDIR)/radiod

install-Darwin:
	@echo "Installing for macOS..."
	rsync -a $(DYNAMIC_DRIVERS) $(SODIR)
	ln -sf $(SODIR)/*.so /usr/local/lib
	rsync -a $(DAEMONS) $(DAEMONDIR)
	rsync -a $(EXECS) $(BINDIR)
	sudo chown root $(BINDIR)/monitor
	sudo chmod u+s $(DAEMONDIR)/radiod

install-%:
	@echo "Unsupported operating system: $*"
	@exit 1

clean:
	rm -f *.o *.a *.d *.so $(DAEMONS) $(EXECS) $(DYNAMIC_DRIVERS)

DEPS = $(CFILES:.c=.d) $(OBJS:.o=.d)

-include $(DEPS)

.PHONY: clean all install install-Linux install-Darwin install-% doc dynamics

# keep all .o files
.SECONDARY:

dynamics: $(DYNAMIC_DRIVERS)

# Executables

fft-gen: fft-gen.o
	$(CC) $(LDOPTS) -o $@ $^  -lfftw3f_threads -lfftw3f  $(LDLIBS)

aprs: aprs.o libradio.a
	$(CC) $(LDOPTS) -o $@ $^ $(LDLIBS)

aprsfeed: aprsfeed.o libradio.a
	$(CC) $(LDOPTS) -o $@ $^ $(LDLIBS)

control: control.o libradio.a
	$(CC) $(LDOPTS) -o $@ $^ -liniparser -lncursesw $(LDLIBS)

cwd: cwd.o libradio.a
	$(CC) $(LDOPTS) -o $@ $^ $(LDLIBS)

tune: tune.o libradio.a
	$(CC) $(LDOPTS) -o $@ $^ $(LDLIBS)

setfilt: setfilt.o libradio.a
	$(CC) $(LDOPTS) -o $@ $^ $(LDLIBS)

show-pkt: show-pkt.o libradio.a
	$(CC) $(LDOPTS) -o $@ $^ -lncursesw $(LDLIBS)

show-sig: show-sig.o libradio.a
	$(CC) $(LDOPTS) -o $@ $^ -lncursesw $(LDLIBS)

metadump: metadump.o libradio.a
	$(CC) $(LDOPTS) -o $@ $^ $(LDLIBS)

monitor: monitor.o monitor-data.o monitor-display.o monitor-repeater.o libradio.a
	$(CC) $(LDOPTS) -o $@ $^ -lopus -lportaudio `pkg-config --silence-errors --libs alsa` -lsamplerate -liniparser  -lncursesw $(LDLIBS)

opusd: opusd.o libradio.a
	$(CC) $(LDOPTS) -o $@ $^ -lopus $(LDLIBS)

opussend: opussend.o libradio.a
	$(CC) $(LDOPTS) -o $@ $^ -lopus -lportaudio $(LDLIBS)

packetd: packetd.o libradio.a
	$(CC) $(LDOPTS) -o $@ $^ -lfftw3f_threads -lfftw3f $(LDLIBS)

pcmcat: pcmcat.o libradio.a
	$(CC) $(LDOPTS) -o $@ $^ $(LDLIBS)

pcmspawn: pcmspawn.o libradio.a
	$(CC) $(LDOPTS) -o $@ $^ $(LDLIBS)

pcmrecord: pcmrecord.o libradio.a
	$(CC) $(LDOPTS) -o $@ $^ -lopus -logg $(LDLIBS)

pcmsend: pcmsend.o libradio.a
	$(CC) $(LDOPTS) -o $@ $^ -lportaudio $(LDLIBS)

powers: powers.o libradio.a
	$(CC) $(LDOPTS) -o $@ $^ $(LDLIBS)

pl: pl.o libradio.a
	$(CC) $(LDOPTS) -o $@ $^ -lfftw3f_threads -lfftw3f $(LDLIBS)

radiod: $(RADIOD_OBJECTS)
	$(CC) -rdynamic $(LDOPTS) -o $@ $^  -lopus -lfftw3f_threads -lfftw3f -liniparser -lusb-1.0 -ldl $(LDLIBS)

rdsd: rdsd.o libradio.a
	$(CC) $(LDOPTS) -o $@ $^ -lfftw3f_threads -lfftw3f $(LDLIBS)

stereod: stereod.o libradio.a
	$(CC) $(LDOPTS) -o $@ $^ -lfftw3f_threads -lfftw3f $(LDLIBS)

jt-decoded: jt-decoded.o libradio.a
	$(CC) $(LDOPTS) -o $@ $^ $(LDLIBS)

wd-record: wd-record.o libradio.a
	$(CC) $(LDOPTS) -o $@ $^ $(LDLIBS)

# not made unless we're on a rpi
set_xcvr: set_xcvr.o libradio.a
	$(CC) $(LDOPTS) -o $@ $^ -lpigpio -liniparser

# Dynamically loaded modules for SDR front ends
funcube.so: funcube.o fcd.o hid-libusb.o
	$(CC) $(SOFLAGS) -o $@ $^ -lportaudio

rx888.so: rx888.o ezusb.o hid-libusb.o
	$(CC) $(SOFLAGS) -o $@ $^ -lusb-1.0

airspy.so: airspy.o
	$(CC) $(SOFLAGS) -o $@ $^ -lairspy

airspyhf.so: airspyhf.o
	$(CC) $(SOFLAGS) -o $@ $^ -lairspyhf

hydrasdr.so: hydrasdr.o
	$(CC) $(SOFLAGS) -o $@ $^ -lhydrasdr

rtlsdr.so: rtlsdr.o
	$(CC) $(SOFLAGS) -o $@ $^ -lrtlsdr

sdrplay.so: sdrplay.o
	$(CC) $(SOFLAGS) -o $@ $^ -lsdrplay_api

fobos.so: fobos.o
	$(CC) $(SOFLAGS) -o $@ $^ -lfobos

hackrf.so: hackrf.o
	$(CC) $(SOFLAGS) -o $@ $^ -lhackrf -lusb-1.0

bladerf.so: bladerf.o
	$(CC) $(SOFLAGS) -o $@ $^ -lbladeRF

# subroutines useful in more than one program
libradio.a: $(LIBRADIO)
	ar rv $@ $?
	ranlib $@

%.so: %.o
	$(CC) $(SOFLAGS) -o $@ $<
