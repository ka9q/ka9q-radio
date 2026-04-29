# -*- makefile -*-

# sub-Makefile for systemd service files
# Copyright 2025, Phil Karn, KA9Q

prefix        ?= /usr/local
sysconfdir    ?= /etc
udevdir	      ?= $(sysconfdir)/udev/rules.d

UNAME_S := $(shell uname -s)

.PHONY: clean all install install-system

all:
	@true
clean:
	@true

install-system: install	

install:
	install -d -m 0755 $(DESTDIR)$(udevdir)
	install -m 0644 *.rules $(DESTDIR)$(udevdir)



