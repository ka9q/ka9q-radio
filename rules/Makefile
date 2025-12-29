# -*- makefile -*-

# sub-Makefile for systemd service files
# Copyright 2025, Phil Karn, KA9Q

UNAME_S := $(shell uname -s)

all:

clean:

install: install-$(UNAME_S)

install-Linux:
	rsync *.rules /etc/udev/rules.d

install-Darwin:
	# TODO
	@exit 0

install-%:
	@echo "Unsupported operating system: $*"
	@exit 1

.PHONY: clean all install install-Linux install-Darwin install-%
