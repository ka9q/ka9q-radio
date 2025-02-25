# Installing ka9q-radio

This package is designed for Debian Linux, including offshoots such as Ubuntu and the Raspberry Pi OS.

Since I use a Macbook Pro as my desktop, some of it (e.g., the interactive components `control` and `monitor`) will also compile and run on MacOS - but not all of it.

However, I'm interested in fixing any unnecessary non-portabilities.

## Prerequisites

To build and install this package on Debian (including the Raspberry Pi), install the prerequisite packages:

```
sudo apt install avahi-utils build-essential make gcc libairspy-dev libairspyhf-dev libavahi-client-dev libbsd-dev libfftw3-dev libhackrf-dev libiniparser-dev libncurses5-dev libopus-dev librtlsdr-dev libusb-1.0-0-dev libusb-dev portaudio19-dev libasound2-dev uuid-dev rsync libogg-dev libsamplerate-dev libliquid-dev libncursesw5-dev
```

(libliquid-dev isn't actually used yet, but it probably will be soon.)

And additionally on the Raspberry Pi:

```
sudo apt install libpigpio-dev
```

Although not needed to build ka9q-radio, I find it useful to install the following:

```
sudo apt install sox libsox-fmt-all opus-tools flac tcpdump wireshark
```

And if your system is in a remote location, I *strongly* recommend this one:

```
sudo apt install molly-guard
```

## Compiling and Installing

```
cd ka9q-radio
ln -s Makefile.[linux|osx] Makefile
make
sudo make install
```

This will write into the following directories:

- **/usr/local/sbin** daemon binaries (e.g., `radiod`)
- **/usr/local/bin** application programs (e.g., `control`)
- **/usr/local/share/ka9q-radio** support files (e.g., `modes.conf`)
- **/var/lib/ka9q-radio** application state files (e.g., FFTW `wisdom` files)
- **/etc/systemd/system** systemd unit files (e.g., `radio@.service`)
- **/etc/sysctl.d** system configuration files (e.g., `98-sockbuf.conf`)
- **/etc/udev/rules.d** device daemon rule files (e.g., `52-airspy.rule`)
- **/etc/fftw** Global (not just ka9q-radio) FFTW *wisdom* files (i.e., `wisdomf`)
- **/etc/radio** program config files (e.g., `radio@2m.conf` - but **will not** overwrite existing files)

It will also create a special system user and group (*radio*) so that the daemons don't have to run with root permissions.

I recommend that you add your own user ID to the *radio* group so you can modify most of the relevant installed directories and files without becoming root:

```
sudo adduser your_user_name radio
```

Membership in a few other groups can minimize the need to run as root:

- *adm* Look at **/var/log/syslog**
- *plugdev* Run `radiod` under a debugger with most devices
- *users* Run `radiod` under a debugger with the Funcube dongle

And of course you need to be a member of the *sudo* group to execute programs as root.

## Configuring *radiod*

This is the most complex part of the setup, and I'm still writing documentation.

A quick-start guide still needs to be written, but you should start with the examples in the **config** subdirectory.

Although the list of available options is quite long, most are rarely needed so most of the examples are actually fairly short and easy to understand.

For ease of maintenance, each configuration may optionally be broken up into sections in a subdirectory.

If `radiod` is given the config file name **radiod@foo.conf**, it will look for the directory named **radiod@foo.conf.d** and read any files found therein after the original configuration file.

Only filenames ending in **.conf** will be used, and they will be sorted by name before being read.

The usual Linux convention is to use names of the form **01-bar.conf** that will sort by the numeric prefix.

Editor temporaries and backups, e.g., **01-bar.conf~**, are ignored.

Config file sections, e.g., `[global]`, `[hardware]`, and so on, may actually appear in any order so the sort order of the subdirectory is important only if a single section is split across two or more files in the subdirectory.

Note that the subdirectory is read *in addition to* the primary config file if it also exists, so to prevent confusion it is advisable to move it to a backup name, e.g., **radiod@foo.conf-disabled** so it will not be read.

The configuration options, including the rarely used esoteric ones, are fully documented here:

- [ka9q-radio.md](ka9q-radio.md) - Management by *systemd*; the [global] section of the `radiod` config file
- [ka9q-radio-2.md](ka9q-radio-2.md) - The hardware dependent section of the `radiod` config file
- [ka9q-radio-3.md](ka9q-radio-3.md) - Setting up static radio channels

## Running *radiod*

### The *radiod* Daemon

What follows assumes some knowledge of common Linux commands and system administration, particularly configuring, starting and stopping daemons running under the standard Linux system management daemon `systemd`.

*ka9q-radio* uses Linux conventions as much as possible.

The core of *ka9q-radio* is the radio daemon, `radiod`.

Like all daemons, `radiod` automatically runs in the background.

Users talk to it only over the network with the client programs `control` and `monitor` (if at all - most *ka9q-radio* applications are completely automatic once configured).

Several instances of `radiod` may run at the same time, subject to resource limits (USB, Ethernet and CPU capacity).

Front ends cannot be shared between `radiod` instances, and each instance can only handle one front end.

Running `make install` in the *ka9q-radio* source directory creates **/usr/local/sbin/radiod** and the `systemd` service file **/etc/systemd/systemd/radiod@.service**.

`systemd` replaces the *@* character in a running instance with the instance name. This file usually need not be modified.

`radiod` reads its configuration from **/etc/radio/radiod@foo.conf**, where **foo** is the instance name.

You should pick a meaningful or descriptive instance name, e.g, **hf**.

Note that **.conf** is *not* part of the instance name; it's a common error to start **radiod@hf.conf** when you mean **radiod@hf**.

The main `systemctl` commands for controlling `radiod` (or any other `systemd` service with multiple instances) are:

```
sudo systemctl start radiod@foo
sudo systemctl stop radiod@foo
sudo systemct restart radiod@foo
sudo systemct enable radiod@foo
sudo systemct disable radiod@foo
systemctl status radiod@foo
```

The first two commands immediately start and stop `radiod`.

`systemctl restart` is equivalent to a `systemctl stop` immediately followed by a `systemctl start`.

The `enable` and `disable` commands have no immediate effect; they configure `systemd` to start `radiod` after a boot, or to prevent that from happening.

It does this by creating or deleting a symbolic link in **/etc/systemd/system/multi-user.target.wants**. Again, this is standard Linux stuff.

Like most system daemons, `radiod` writes startup and error messages to the standard system log, **/var/log/syslog**.

You can read this file directly (e.g., with `grep` or `tail`) or through the `systemctl status` command.

You do not have to be root to run `systemctl status` but you usually have to be root or a member of group *adm* to read **/var/log/syslog**.

Note that `systemctl status` only gives you the last 10 lines of output from `radiod`.

### Client Programs

## Optimizations

Read the file [FFTW3.md](FFTW3.md) on pre-computing efficient transforms for the FFTs in `radiod`.
