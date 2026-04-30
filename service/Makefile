# -*- makefile -*-
# sub-make for ka9q-radio systemd .service files
# Copyright 2026, Phil Karn, KA9Q

systemdunitdir ?= /etc/systemd/system

SERVICEFILES=acars aprsfeed cwd ft4-decode ft8-decode ft4-decode@ ft8-decode@ ft4-record ft8-record hfdl horusdemod packetd \
	radiod@ recordings@ rx888_boot set_lo_multicast wspr-decoded


.PHONY: clean all install install-system

all:
	@true

clean:
	@true

# install as ordinary files for now, don't have dpkg autostart them
install:
	install -d 0755 $(DESTDIR)$(systemdunitdir)
	install -m 0644 *.service $(DESTDIR)$(systemdunitdir)

install-system:
	@true


service/%.service: service/%.service.in
	sed -e 's|@bindir@|$(bindir)|g' \
	    -e 's|@sbindir@|$(sbindir)|g' \
	    $< > $@

show-service-paths:
	@echo bindir=$(bindir)
	@echo sbindir=$(sbindir)
	@echo systemdunitdir=$(systemdunitdir)
