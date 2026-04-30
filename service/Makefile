# -*- makefile -*-
# sub-make for ka9q-radio systemd .service files
# Copyright 2026, Phil Karn, KA9Q

systemdunitdir ?= /etc/systemd/system
bindir         ?= /usr/local/bin
sbindir        ?= /usr/local/sbin

SERVICEFILES=acars.service aprsfeed.service cwd.service ft4-decode.service ft8-decode.service \
	ft4-decode@.service ft8-decode@.service ft4-record.service ft8-record.service \
	hfdl.service horusdemod.service  packetd.service radiod@.service recordings@.service \
	rx888_boot.service set_lo_multicast.service wspr-decoded.service


.PHONY: clean all install install-system

all: $(SERVICEFILES)

clean:
	rm -f *.service

# install as ordinary files for now, don't have dpkg autostart them
install-system: install

install: $(SERVICEFILES)
	install -d 0755 $(DESTDIR)$(systemdunitdir)
	install -m 0644 $(SERVICEFILES) $(DESTDIR)$(systemdunitdir)

%.service: %.service.in
	sed -e 's|@bindir@|$(bindir)|g' \
	    -e 's|@sbindir@|$(sbindir)|g' \
	    $< > $@

show-service-paths:
	@echo bindir=$(bindir)
	@echo sbindir=$(sbindir)
	@echo systemdunitdir=$(systemdunitdir)
