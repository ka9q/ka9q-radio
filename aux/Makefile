# -*- makefile -*-

# sub-make for ka9q aux files
# Copyright 2025, Phil Karn, KA9Q

UNAME_S := $(shell uname -s)

DAEMONDIR = /usr/local/sbin
VARDIR = /var/lib/ka9q-radio
BINDIR = /usr/local/bin
DIRS = /etc/fftw /etc/radio /etc/avahi/services $(VARDIR) /var/lib/hfdl $(VARDIR)/ft8 $(VARDIR)/ft4 $(VARDIR)/wspr /var/log/acars
SYSFILES = /etc/avahi/hosts /var/log/hfdl.log
SCRIPTS = set_lo_multicast.sh
LOGROTATE_FILES = aprsfeed.rotate ft8.rotate ft4.rotate wspr.rotate hfdl.rotate recordings.rotate
BLACKLIST = airspy-blacklist.conf
SYSCTL = 98-sockbuf.conf 50-multicast.conf

all:

clean:

install: install-$(UNAME_S)

install-Linux:
	# Create radio group
	/usr/sbin/adduser --quiet --system --group radio
	# Setup init system
	install -d -o root -g root -m 0755 /etc/sysusers.d /etc/tmpfiles.d
	install -o root -g root -m 0644 radio.sysusers /etc/sysusers.d/radio.conf
	install -o root -g root -m 0644 ka9q-radio.tmpfiles /etc/tmpfiles.d/ka9q-radio.conf
	# Setup ka9q-radio directories
	mkdir -p $(DIRS)
	-chown -R radio:radio $(DIRS)
	-chmod g+ws $(DIRS)
	touch $(SYSFILES)
	chgrp radio $(SYSFILES)
	chmod g+w $(SYSFILES)
	# Setup kernel configuration
	rsync $(SYSCTL) /etc/sysctl.d
	sysctl -q -p $(SYSCTL)
	rsync $(BLACKLIST) /etc/modprobe.d
	# Setup log rotation
	rsync $(LOGROTATE_FILES) /etc/logrotate.d
	chown root:root /etc/logrotate.d/*
	chmod go-w /etc/logrotate.d/*
	# Configure HFDL
	rsync --ignore-existing systable.conf /var/lib/hfdl
	rsync start-hfdl.sh $(DAEMONDIR)
	rsync ka9q-cleanups /etc/cron.d/
	# Configure cron jobs
	chown root:root /etc/cron.d/ka9q-cleanups
	chmod 0644 /etc/cron.d/ka9q-cleanups
	# Configure scripts
	rsync $(SCRIPTS) $(BINDIR)

install-Darwin:
	# Create radio group
	sudo dscl . -create /Groups/radio PrimaryGroupID 492
	# Setup init system (TBD)
	# Setup ka9q-radio directories
	mkdir -p $(DIRS)
	-chgrp -R radio $(DIRS)
	-chmod g+ws $(DIRS)
	touch $(SYSFILES)
	chgrp radio $(SYSFILES)
	chmod g+w $(SYSFILES)
	# Setup kernel configuration (TBD)
	# Setup log rotation (TBD)
	# Configure HFDL
	rsync --ignore-existing systable.conf /var/lib/hfdl
	rsync start-hfdl.sh $(DAEMONDIR)
	rsync ka9q-cleanups /etc/cron.d/
	# Configure cron jobs (TBD)
	# Configure scripts
	rsync $(SCRIPTS) $(BINDIR)

install-%:
	@echo "Unsupported operating system: $*"
	@exit 1

.PHONY: clean all install install-Linux install-Darwin install-%
