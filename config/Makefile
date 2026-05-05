# sub-make for ka9q config files
# Copyright 2026, Phil Karn, KA9Q
sysconfdir    ?= /etc
radioconfdir  ?= $(sysconfdir)/radio

UNAME_S := $(shell uname -s)

.PHONY: clean all install purge

all:
	@true

clean:
	@true

purge:
	@test "$(CONFIRMED)" = "1" || { echo "Refusing to run purge directly. Use top-level make purge."; exit 1; }
	if [ -d "$(DESTDIR)$(radioconfdir)" ]; then \
		rm -rf $(DESTDIR)$(radioconfdir); \
	fi

# Setup sample config files
# don't overwrite the user's files when locally installing
install:
	install -d -m 0775 $(DESTDIR)$(radioconfdir)
ifndef DEB_BUILD_ARCH
	rsync -a --ignore-existing README *.conf *.conf.d $(DESTDIR)$(radioconfdir)
	chown radio:radio $(DESTDIR)$(radioconfdir)
else
	rsync -a README *.conf *.conf.d $(DESTDIR)$(radioconfdir)
	@true
endif
