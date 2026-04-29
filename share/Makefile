# -*- makefile -*-

# sub-Make for ka9q-radio share files
# Copyright 2025, Phil Karn, KA9Q

prefix        ?= /usr/local
datadir       ?= $(prefix)/share
pkgdatadir    ?= $(datadir)/ka9q-radio

UNAME_S := $(shell uname -s)

RX888=120s_failure.img 20s_watchdog.img	120s_watchdog.img 60s_watchdog.img SDDC_FX3.img
LUA=ssrc-decimal.lua ka9q_ctl.lua
TEXT=bandplan.txt id.txt presets.conf help.txt monitor-help.txt


.PHONY: clean all install install-system

all:
	@:

clean:
	@:

install-system: install
	chown radio:radio $(RX888) $(LUA) $(TEXT) $(DESTDIR)$(pkgdatadir)

install:
	install -d -m 0775 $(DESTDIR)$(pkgdatadir)
	install -m 0664 $(RX888) $(LUA) $(TEXT) $(DESTDIR)$(pkgdatadir)


