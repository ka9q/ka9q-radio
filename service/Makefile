# -*- makefile -*-
# sub-make for ka9q-radio systemd .service files
# Copyright 2026, Phil Karn, KA9Q

systemunitdir ?= /etc/systemd/system

.PHONY: clean all install install-system

all:
	@true

clean:
	@true

# install as ordinary files for now, don't have dpkg autostart them
install:
	install -d 0755 $(DESTDIR)$(systemunitdir)
	install *.service $(DESTDIR)$(systemunitdir)

install-system:
	@true

