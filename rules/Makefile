# sub-Makefile for systemd service files
# Copyright 2026, Phil Karn, KA9Q

udevdir	      ?= /etc/udev/rules.d

UNAME_S := $(shell uname -s)
RULES = 20-rtlsdr.rules 51-airspy.rules 51-hydrasdr.rules 52-airspyhf.rules 53-airspy-ka9q.rules 66-hackrf.rules 66-sdrplay.rules \
	68-funcube-dongle-proplus.rules 68-funcube-dongle.rules 69-funcube-ka9q.rules 70-rx888-boot.rules 71-rx888.rules \
	72-rx888-ka9q.rules 73-fobos-sdr.rules 74-fobos-sdr-ka9q.rules

.PHONY: clean all install uninstall purge

all:
	@true
clean:
	@true

install:
	install -d -m 0755 $(DESTDIR)$(udevdir)
	install -m 0644 $(RULES) $(DESTDIR)$(udevdir)

uninstall:
	rm -f $(addprefix $(DESTDIR)$(udevdir)/,$(RULES))

purge:
	@test "$(CONFIRMED)" = "1" || { \
	    echo "Refusing to run purge directly. Use top-level make purge."; \
	    exit 1; \
	}
	if [ -d "$(DESTDIR)$(udevdir)" ]; then \
		cd $(DESTDIR)$(udevdir) && rm -f $(RULES); \
	fi

