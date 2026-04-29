# -*- makefile -*-
# sub-make for ka9q-radio systemd .service files
# Copyright 2025, Phil Karn, KA9Q

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
servicedir    ?= $(sysconfdir)/systemd/system

UNAME_S := $(shell uname -s)

.PHONY: clean all install install-system

all:
	@true

clean:
	@true

install-system: install

install:
	install -d 0755 $(DESTDIR)$(servicedir)
	install *.service $(DESTDIR)$(servicedir)



