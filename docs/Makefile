# Copyright 2026, Phil Karn, KA9Q

prefix	?= /usr/local
mandir  ?= $(prefix)/share/man

PAGES1 = aprs.1 control.1 cwd.1 fft-gen.1 jt-decoded.1 metadump.1 monitor.1 pcmrecord.1 ctcss.1 powers.1 tune.1 wd-record.1
PAGES8 = radiod.8 aprsfeed.8 packetd.8 

.PHONY: clean all install

all:
	@true

clean:
	@true

install:
	install -d -m 0755 $(DESTDIR)$(mandir) $(DESTDIR)$(mandir)/man1 $(DESTDIR)$(mandir)/man8
	install -m 0644 $(PAGES1) $(DESTDIR)$(mandir)/man1
	install -m 0644 $(PAGES8) $(DESTDIR)$(mandir)/man8

