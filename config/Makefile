# sub-make for ka9q config files
# Copyright 2026, Phil Karn, KA9Q
sysconfdir    ?= /etc
radioconfdir  ?= $(sysconfdir)/radio

UNAME_S := $(shell uname -s)

.PHONY: clean all install install-system

all:
	@true

clean:
	@true

install-system: install
	chown radio:radio $(DESTDIR)$(radioconfdir)

install:
# Setup sample config files
	install -d -m 0775 $(DESTDIR)$(radioconfdir)
	rsync -a *.conf *.conf.d $(DESTDIR)$(radioconfdir)
