# -*- makefile -*-

# sub-make for ka9q aux files
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
radioconfdir  ?= $(sysconfdir)/radio
avahidir        ?= $(sysconfdir)/avahi
logrotatedir    ?= $(sysconfdir)/logrotate.d
sysctldir       ?= $(sysconfdir)/sysctl.d
modprobedir     ?= $(sysconfdir)/modprobe.d
sysusersdir     ?= $(sysconfdir)/sysusers.d
hfdllibdir	?= $(libdir)/hfdl
crondir         ?= $(sysconfdir)/cron.d

UNAME_S := $(shell uname -s)

SCRIPTS = set_lo_multicast start-ka9q-horus
LOGROTATE_FILES = aprsfeed.rotate ft8.rotate ft4.rotate wspr.rotate hfdl.rotate recordings.rotate
BLACKLIST = airspy-blacklist.conf
SYSCTL = 98-sockbuf.conf 50-multicast.conf

.PHONY: clean all install install-system

all:
	@true

clean:
	@true

install-system: install
	install -D -m 0775 $(DESTDIR)$(radioconfdir)
	chown radio:radio $(DESTDIR)$(radioconfdir) $(DESTDIR)$(hfdllibdir) $(DESTDIR)$(statedir) \
		$(DESTDIR)$(statedir)/ft8 $(DESTDIR)$(statedir)/ft4 $(DESTDIR)$(statedir)/wspr $(DESTDIR)$(hfdllibdir) 

install:
# Setup ka9q-radio directories
	install -d -m 0775 $(DESTDIR)$(hfdllibdir)
	install -d -m 0755 $(DESTDIR)$(sysusersdir) $(DESTDIR)$(sysctldir) $(DESTDIR)$(modprobedir) \
		$(DESTDIR)$(logrotatedir) $(DESTDIR)$(sbindir) $(DESTDIR)$(crondir) $(DESTDIR)$(bindir)

# Setup init system
	install -m 0644 radio.sysusers $(DESTDIR)$(sysusersdir)/radio.conf
# Setup kernel configuration
	install -m 0644 $(SYSCTL) $(DESTDIR)$(sysctldir)
	install -m 0644 $(BLACKLIST) $(DESTDIR)$(modprobedir)
# Setup log rotation
	install -m 0644 $(LOGROTATE_FILES) $(DESTDIR)$(logrotatedir)
# Configure HFDL
	install -m 0644 systable.conf $(DESTDIR)$(hfdllibdir)
	install -m 0755 start-hfdl $(DESTDIR)$(sbindir)
# Configure cron jobs
	install -m 0644 ka9q-cleanups $(DESTDIR)$(crondir)
# Configure scripts
	install -m 0755 $(SCRIPTS) $(DESTDIR)$(bindir)


