# Copyright 2026, Phil Karn, KA9Q

prefix	?= /usr/local
mandir  ?= $(prefix)/share/man


.PHONY: clean all install

all:
	@true

clean:
	@true

install:
	install -m 0644 monitor.1 control.1 $(DESTDIR)$(mandir)/man1
	install -m 0644 radiod.8 $(DESTDIR)$(mandir)/man8

