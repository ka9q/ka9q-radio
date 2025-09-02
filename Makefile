# -*- makefile -*-

# Primary Linux makefile for ka9q-radio package
# Copyright 2025, Phil Karn, KA9Q

SUBDIRS=src aux share service rules docs

all: $(SUBDIRS)

$(SUBDIRS):
	$(MAKE) -C $@

clean install:
	for d in $(SUBDIRS); do \
		$(MAKE) -C $$d $@ || exit $$?; \
	done

.PHONY: clean all install $(SUBDIRS)




