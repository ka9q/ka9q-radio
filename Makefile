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

# set those for which Debian packages are already available
# 
ENABLE_AIRSPY   ?= 1
ENABLE_AIRSPYHF ?= 1
ENABLE_BLADERF  ?= 1
ENABLE_FOBOS    ?= 1
ENABLE_FUNCUBE  ?= 1
ENABLE_HACKRF   ?= 1
ENABLE_HYDRASDR ?= 1
ENABLE_RTLSDR   ?= 1
ENABLE_RX888    ?= 1
ENABLE_SDRPLAY  ?= 0

export ENABLE_AIRSPY ENABLE_AIRSPYHF ENABLE_BLADERF ENABLE_FOBOS ENABLE_FUNCUBE ENABLE_HACKRF ENABLE_HYDRASDR
export ENABLE_RTLSDR ENABLE_RX888 ENABLE_SDRPLAY
export DEB_BUILD_ARCH

SUBDIRS=src aux share service rules docs config
.PHONY: all clean install $(SUBDIRS) purge

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

# only do system stuff when installing locally
# dpkg-buildpackage does that when building a debian package
install:
ifndef DEB_BUILD_ARCH
ifeq ($(UNAME_S),Linux)
	getent group radio >/dev/null || groupadd --system radio
	id radio >/dev/null 2>&1 || useradd --system --gid radio --home-dir /var/lib/ka9q-radio --no-create-home radio
else
	@echo "Skipping automatic id/group creation on non-Linux system"
endif
	for d in $(SUBDIRS); do \
		$(MAKE) -C $$d install DESTDIR=$(DESTDIR) || exit $$?; \
	done
ifeq ($(UNAME_S),Linux)
	systemctl daemon-reload
	udevadm control --reload-rules
	setcap cap_net_admin+ep $(bindir)/monitor
endif
endif
