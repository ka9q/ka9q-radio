# sub-make for ka9q config files
# Copyright 2026, Phil Karn, KA9Q
sysconfdir    ?= /etc
radioconfdir  ?= $(sysconfdir)/radio

UNAME_S := $(shell uname -s)

.PHONY: clean all install

all:
	@true

clean:
	@true

# Setup sample config files
# don't overwrite the user's files when locally installing
install:
ifndef DEB_BUILD_ARCH
	install -d -m 0775 $(DESTDIR)$(radioconfdir)
	rsync -a --ignore-existing *.conf *.conf.d $(DESTDIR)$(radioconfdir)
	chown radio:radio $(DESTDIR)$(radioconfdir)
else
	@true
endif
