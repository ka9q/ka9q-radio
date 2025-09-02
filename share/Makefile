# -*- makefile -*-

# sub-Make for ka9q-radio share files
# Copyright 2025, Phil Karn, KA9Q

LIBDIR = /usr/local/share/ka9q-radio

all:

clean:

install:
	rsync --ignore-existing * $(LIBDIR)
	chown radio:radio $(LIBDIR)

.PHONY: clean all install


