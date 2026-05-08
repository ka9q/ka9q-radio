# sub-make for ka9q config files
# Copyright 2026, Phil Karn, KA9Q
prefix        ?= /usr/local
sysconfdir    ?= /etc
radioconfdir  ?= $(sysconfdir)/radio
datadir       ?= $(prefix)/share
pkgdatadir    ?= $(datadir)/ka9q-radio

UNAME_S := $(shell uname -s)

.PHONY: clean all install purge

all:
	@true

purge:
	echo "not cleaning /etc/radio"

install:
	install -m 0664 *.conf $(DESTDIR)$(radioconfdir)
	rsync -a examples $(DESTDIR)$(pkgdatadir)
