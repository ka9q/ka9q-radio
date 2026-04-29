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
PKG_CONFIG_PATH=
LD_LIBRARY_PATH=

# set those for which Debian packages are already available
# 
ENABLE_AIRSPY   ?= 1
ENABLE_AIRSPYHF ?= 1
ENABLE_BLADERF  ?= 1
ENABLE_FOBOS    ?= 0
ENABLE_FUNCUBE  ?= 1
ENABLE_HACKRF   ?= 1
ENABLE_HYDRASDR ?= 0
ENABLE_RTLSDR   ?= 1
ENABLE_RX888    ?= 1
ENABLE_SDRPLAY  ?= 0

SUBDIRS=src aux share service rules docs config
.PHONY: clean all install $(SUBDIRS) install-system commands

all: $(SUBDIRS)

$(SUBDIRS):
	$(MAKE) -C $@

clean install install-system:
	for d in $(SUBDIRS); do \
		$(MAKE) -C $$d $@ DESTDIR=$(DESTDIR) || exit $$?; \
	done

install-system: userids install-system commands

userids:
	getent group radio >/dev/null || groupadd --system radio
	id radio >/dev/null 2>&1 || useradd --system --gid radio \
		--home-dir /var/lib/ka9q-radio --no-create-home radio

commands:
	systemctl daemon-reload
	udevadm control --reload-rules
	setcap cap_net_admin+ep $(bindir)/monitor
