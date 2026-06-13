# -*- makefile -*-

# Top-level Linux makefile for ka9q-radio package
# Copyright 2026, Phil Karn, KA9Q
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
mandir	      ?= $(prefix)/share/man

UNAME_S := $(shell uname -s)

export prefix exec_prefix bindir sbindir libdir datadir sysconfdir
export localstatedir pkgdatadir pkglibdir statedir mandir

PKG_CONFIG_PATH=
LD_LIBRARY_PATH=

# set those for which Debian device library packages are available
# on the build machine. Because support is compiled as .so plugins
# device libraries do not need to be installed on the target system
# if that device isn't going to be used. The .so files are broken out
# into device-specific packages (eg, ka9q-radio-rx888) and the library
# dependency is enforced when those packages are installed.
ENABLE_AIRSPY   ?= 1
ENABLE_AIRSPYHF ?= 1
ENABLE_BLADERF  ?= 1
ENABLE_FOBOS    ?= 1
ENABLE_FUNCUBE  ?= 1
ENABLE_HACKRF   ?= 1
ENABLE_HYDRASDR ?= 1
ENABLE_RTLSDR   ?= 1
ENABLE_RX888    ?= 1
ENABLE_SDRPLAY  ?= 0  # this is the really problematic one: proprietary API
ENABLE_SIG_GEN  ?= 1

export ENABLE_AIRSPY ENABLE_AIRSPYHF ENABLE_BLADERF ENABLE_FOBOS
export ENABLE_FUNCUBE ENABLE_HACKRF ENABLE_HYDRASDR
export ENABLE_RTLSDR ENABLE_RX888 ENABLE_SDRPLAY ENABLE_SIG_GEN
export DEB_BUILD_ARCH

SUBDIRS=src aux share service rules docs config
.PHONY: all clean install uninstall $(SUBDIRS) purge

all: $(SUBDIRS)

$(SUBDIRS):
	$(MAKE) -C $@

clean:
	for d in $(SUBDIRS); do \
		$(MAKE) -C $$d clean DESTDIR=$(DESTDIR) || exit $$?; \
	done

purge:
	@if [ "$(CONFIRMED)" != "1" ]; then \
	    printf "This will delete installed ka9q-radio files (including configs). Continue? [y/N] "; \
	    read ans; \
	    case "$$ans" in \
	        y|Y|yes|YES) ;; \
	        *) echo "Aborted."; exit 1 ;; \
	    esac; \
	fi
	@for d in $(SUBDIRS); do \
	    echo $$d:; \
	    $(MAKE) -C $$d purge CONFIRMED=1 || exit $$?; \
	done

# the ka9q-radio-common package now creates the radio user and group
install:
	for d in $(SUBDIRS); do \
		$(MAKE) -C $$d install DESTDIR=$(DESTDIR) || exit $$?; \
	done

uninstall:
	for d in $(SUBDIRS); do $(MAKE) -C $$d uninstall; done

