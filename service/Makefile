# -*- makefile -*-
# sub-make for ka9q-radio systemd .service files
# Copyright 2025, Phil Karn, KA9Q

all:

clean:

install:
	rsync -a * /etc/systemd/system
	chown root:root /etc/systemd/system
	systemctl daemon-reload

.PHONY: clean all install


