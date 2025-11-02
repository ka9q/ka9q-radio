# -*- makefile -*-

# sub-Make for ka9q-radio share files
# Copyright 2025, Phil Karn, KA9Q

UNAME_S := $(shell uname -s)

LIBDIR = /usr/local/share/ka9q-radio

all:

clean:

install: install-$(UNAME_S)

install-Linux:
	rsync --ignore-existing * $(LIBDIR)
	chown radio:radio $(LIBDIR)

install-Darwin:
	rsync --ignore-existing * $(LIBDIR)
	chgrp radio $(LIBDIR)

install-%:
        @echo "Unsupported operating system: $*"
        @exit 1

.PHONY: clean all install install-Linux install-Darwin install-%
