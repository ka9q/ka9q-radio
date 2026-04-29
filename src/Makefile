# -*- makefile -*-

# Primary Linux makefile for ka9q-radio package
# Copyright 2017-2025, Phil Karn, KA9Q
# By default, builds release version
# To build for debugging say "make BUILD=debug"

BUILD ?= release
prefix        ?= /usr/local
exec_prefix   ?= $(prefix)
bindir        ?= $(exec_prefix)/bin
sbindir       ?= $(exec_prefix)/sbin
libdir        ?= $(exec_prefix)/lib
datadir       ?= $(prefix)/share
sysconfdir    ?= /etc
localstatedir ?= /var
pkgdatadir    ?= $(datadir)/ka9q-radio
pkglibdir     ?= $(libdir)/ka9q-radio
statedir      ?= $(localstatedir)/lib/ka9q-radio

UNAME_S := $(shell uname -s)

CPPFLAGS ?=
LDFLAGS  ?=

LDLIBS = -lm -lpthread

GIT_HASH := $(shell git rev-parse HEAD 2>/dev/null || echo "unknown")
GIT_TIME := $(shell git show -s --format=%ci 2>/dev/null || echo "unknown")
GIT_BRANCH := $(shell git log --pretty=format:'%d' -n 1 2>/dev/null || echo "unknown")
GIT_SUMMARY := $(shell git log -1 --format="%s" 2>/dev/null || echo "unknown")
GIT_VERSION := $(shell git describe --always --dirty --tags 2>/dev/null || echo "unknown")
GIT_REMOTE_URL := $(shell git remote get-url origin 2>/dev/null || echo "unknown")

#VERSION_FLAGS = -DGIT_HASH='"$(GIT_HASH)"' \
#                -DGIT_TIME='"$(GIT_TIME)"' \
#                -DGIT_BRANCH='"$(GIT_BRANCH)"' \
#                -DGIT_SUMMARY='"$(GIT_SUMMARY)"' \
#                -DGIT_VERSION='"$(GIT_VERSION)"' \
#                -DGIT_REMOTE_URL='"$(GIT_REMOTE_URL)"'

VERSION_FLAGS   = -DGIT_HASH='"$(GIT_HASH)"' -DGIT_REMOTE_URL='"$(GIT_REMOTE_URL)"'

ifeq ($(UNAME_S),Darwin)
  CPPFLAGS += -I/opt/local/include
  LDFLAGS  += -L/opt/local/lib
  LDLIBS += -lintl -liconv
  SOFLAGS = -bundle -Wl,-undefined,dynamic_lookup
else
  LDLIBS += -lbsd -lrt
  SOFLAGS = -shared
ifdef DEB_BUILD_ARCH
  CFLAGS   += $(shell dpkg-buildflags --get CFLAGS)
  CPPFLAGS += $(shell dpkg-buildflags --get CPPFLAGS)
  LDFLAGS  += $(shell dpkg-buildflags --get LDFLAGS)
endif
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
COPTS = -std=gnu11 -pthread -Wall -funsafe-math-optimizations -fno-math-errno -fcx-limited-range -freciprocal-math -fno-trapping-math -D_GNU_SOURCE=1 -Wextra -Wno-sign-conversion -Wno-int-conversion -MMD -MP
COPTS += -fPIC
CFLAGS += $(DOPTS) $(ARCHOPTS) $(COPTS) $(INCLUDES) $(VERSION_FLAGS)
DAEMONS = aprs aprsfeed cwd packetd radiod
EXECS = control fft-gen jt-decoded metadump monitor pcmrecord pl powers tune wd-record
LIBRADIO = ax25.o morse.o bandplan.o dump.o modes.o avahi.o avahi_browse.o attr.o filter.o iir.o decode_status.o status.o misc.o multicast.o rtp.o osc.o config.o sincospi.o sincospif.o window.o gauss.o
DYNAMIC_DRIVERS = sig_gen.so
RADIOD_OBJECTS = main.o audio.o fm.o wfm.o linear.o spectrum.o radio.o radio_status.o rtcp.o fcd.o hid-libusb.o libradio.a
CFILES = airspy.c airspyhf.c aprs.c aprsfeed.c attr.c audio.c avahi.c avahi_browse.c ax25.c bandplan.c config.c control.c cwd.c decimate.c decode_status.c dump.c ezusb.c fcd.c filter.c fm.c funcube.c hid-libusb.c iir.c jt-decoded.c linear.c main.c metadump.c misc.c modes.c monitor.c monitor-data.c monitor-display.c monitor-repeater.c morse.c multicast.c opusd.c opussend.c osc.c packetd.c pcmcat.c pcmrecord.c pcmsend.c pcmspawn.c pl.c powers.c radio.c radio_status.c rdsd.c rtcp.c rtlsdr.c rtp.c rx888.c setfilt.c show-pkt.c show-sig.c sig_gen.c spectrum.c status.c stereod.c tune.c wd-record.c wfm.c
HFILES = attr.h ax25.h bandplan.h conf.h config.h decimate.h ezusb.h fcd.h fcdhidcmd.h filter.h hidapi.h iir.h misc.h monitor.h morse.h multicast.h osc.h radio.h rx888.h status.h

# AIRSPY R2, etc
ifeq ($(ENABLE_AIRSPY),1)
   CFILES += airspy.c
   DYNAMIC_DRIVERS += airspy.so
endif

# AIRSPY HF+
ifeq ($(ENABLE_AIRSPYHF),1)
   CFILES += airspyhf.c
   DYNAMIC_DRIVERS += airspyhf.so
endif

# BLADERF
ifeq ($(ENABLE_BLADERF),1)
   CFILES += bladerf.c
   DYNAMIC_DRIVERS += bladerf.so
endif

# Rigexpert Fobos SDR
ifeq ($(ENABLE_FOBOS),1)
   CFILES += fobos.c
   DYNAMIC_DRIVERS += fobos.so
endif

# Old AMSAT-UK Funcube dongle (my first SDR!)
ifeq ($(ENABLE_FUNCUBE),1)
   CFILES += funcube.c
   DYNAMIC_DRIVERS += funcube.so
endif

# HACK RF
ifeq ($(ENABLE_HACKRF),1)
   CFILES += hackrf.c
   DYNAMIC_DRIVERS += hackrf.so
endif

# HydraSDR One
ifeq ($(ENABLE_HYDRASDR),1)
   CFILES += hydrasdr.c
   DYNAMIC_DRIVERS += hydrasdr.so
endif

# cheap RTLSDR dongles
ifeq ($(ENABLE_RTLSDR),1)
   CFILES += rtlsdr.c
   DYNAMIC_DRIVERS += rtlsdr.so
endif

# RX 888 MkII
ifeq ($(ENABLE_RX888),1)
   CFILES += rx888.c
   DYNAMIC_DRIVERS += rx888.so
   EXECS += rx888_boot
endif

ifeq ($(ENABLE_SDRPLAY),1)
   CFILES += sdrplay.c
   DYNAMIC_DRIVERS += sdrplay.so
endif

.PHONY: clean all install doc dynamics

all: $(DAEMONS) $(EXECS) $(DYNAMIC_DRIVERS)

install: $(DAEMONS) $(EXECS) $(DYNAMIC_DRIVERS)
	install -d -m 0755 $(DESTDIR)$(sbindir) $(DESTDIR)$(bindir) $(DESTDIR)$(pkglibdir) \
		$(DESTDIR)$(sbindir) $(DESTDIR)$(bindir)
	install -m 0755 $(DYNAMIC_DRIVERS) $(DESTDIR)$(pkglibdir)
	install -m 0755 $(DAEMONS) $(DESTDIR)$(sbindir)
	install -m 0755 $(EXECS) $(DESTDIR)$(bindir)

clean:
	rm -f *.o *.a *.d *.so $(DAEMONS) $(EXECS) $(DYNAMIC_DRIVERS)

DEPS = $(CFILES:.c=.d) $(OBJS:.o=.d)

-include $(DEPS)

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

rx888_boot: rx888_boot.o ezusb.o
	$(CC) $(LDOPTS) -o $@ $^ -lusb-1.0 $(LDLIBS)

stereod: stereod.o libradio.a
	$(CC) $(LDOPTS) -o $@ $^ -lfftw3f_threads -lfftw3f $(LDLIBS)

jt-decoded: jt-decoded.o libradio.a
	$(CC) $(LDOPTS) -o $@ $^ $(LDLIBS)

wd-record: wd-record.o libradio.a
	$(CC) $(LDOPTS) -o $@ $^ -lopus -logg $(LDLIBS)

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

sig_gen.so: sig_gen.o libradio.a
	$(CC) $(SOFLAGS) -o $@ $^ -lsamplerate

# subroutines useful in more than one program
libradio.a: $(LIBRADIO)
	ar rv $@ $?
	ranlib $@

%.so: %.o
	$(CC) $(SOFLAGS) -o $@ $<
