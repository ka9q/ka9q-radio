# -*- makefile -*-

# sub-make for ka9q aux files
# Copyright 2025, Phil Karn, KA9Q

DAEMONDIR = /usr/local/sbin
VARDIR = /var/lib/ka9q-radio
BINDIR = /usr/local/bin
DIRS = /etc/fftw /etc/radio /etc/avahi/services $(VARDIR) /var/lib/hfdl $(VARDIR)/ft8 $(VARDIR)/ft4 $(VARDIR)/wspr /var/log/acars
SYSFILES = /etc/avahi/hosts /var/log/hfdl.log
SCRIPTS = aux/set_lo_multicast.sh
LOGROTATE_FILES = aprsfeed.rotate ft8.rotate ft4.rotate wspr.rotate hfdl.rotate
BLACKLIST = airspy-blacklist.conf
SYSCTL = 98-sockbuf.conf 50-multicast.conf

all:

clean:

install:
	/usr/sbin/adduser --quiet --system --group radio
	mkdir -p $(DIRS)
	-chown -R radio:radio $(DIRS)
	-chmod g+ws $(DIRS)
	touch $(SYSFILES)
	chgrp radio $(SYSFILES)
	chmod g+w $(SYSFILES)
	rsync $(SYSCTL) /etc/sysctl.d
	sysctl -q -p $(SYSCTL)
	rsync $(BLACKLIST) /etc/modprobe.d
	rsync $(LOGROTATE_FILES) /etc/logrotate.d
	chown root:root /etc/logrotate.d/*
	chmod go-w /etc/logrotate.d/*
	rsync --ignore-existing systable.conf /var/lib/hfdl
	rsync start-hfdl.sh $(DAEMONDIR)
	rsync ka9q-cleanups /etc/cron.d/
	chown root:root /etc/cron.d/ka9q-cleanups
	chmod 0644 /etc/cron.d/ka9q-cleanups
	rsync $(SCRIPTS) $(BINDIR)

.PHONY: clean all install


