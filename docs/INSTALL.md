Installing ka9q-radio  
September 22, 2023, KA9Q
=====================
This package is designed for Debian Linux, including the Raspberry Pi
OS. Since I use a Macbook Pro as my desktop, some of it (e.g., the
interactive components 'control' and 'monitor') will also compile and
run on MacOS -- but not all of it. However, I'm interested in fixing
any unnecessary non-portabilities.

Prerequisites
-------------

To build and install this package on Debian, install the prerequisite packages:

sudo apt install build-essential make gcc libairspy-dev libairspyhf-dev libavahi-client-dev libbsd-dev libfftw3-dev libhackrf-dev libiniparser-dev libncurses5-dev libopus-dev librtlsdr-dev libusb-1.0-0-dev libusb-dev portaudio19-dev libasound2-dev uuid-dev rsync

And on the Raspberry Pi:

sudo apt install libpigpio-dev

Although not needed to build ka9q-radio, I find it useful to install the following:

sudo apt install sox libsox-fmt-all opus-tools flac avahi-utils avahi-utils tcpdump wireshark

And if your system is in a remote location, I strongly recommend this one:

sudo apt install molly-guard


Compiling and Installing
------------------------

$ cd ka9q-radio  
$ ln -s Makefile.[linux|pi] Makefile  
$ make  
$ sudo make install  

This will write into the following directories:

/usr/local/sbin	     	 	   daemon binaries (e.g., 'radiod')  
/usr/local/bin		 	   application programs (e.g., 'control')  
/usr/local/share/ka9q-radio	   support files (e.g., 'modes.conf')  
/var/lib/ka9q-radio		   application state files (e.g., tune-*)  
/etc/systemd/system  		   systemd unit files (e.g., radio@.service)  
/etc/sysctl.d	    		   system configuration files (e.g., 98-sockbuf.conf)  
/etc/udev/rules.d		   device daemon rule files (e.g., 52-airspy.rules)  
/etc/fftw			   FFTW "wisdom" files (i.e., wisdomf)  
/etc/radio			   program config files (e.g., radio@2m.conf - but *will not* overwrite existing files)

It will also create several special system users and groups so that
the daemons don't have to run with root permissions. I recommend that
you add your own user ID to the **radio** group so you can 
modify most of the relevant installed directories and files without
becoming root:

$ sudo addgroup your_user_name radio

Membership in a few other groups can minimize the need to run as root:

**adm** Look at */var/log/syslog  
**plugdev** Run *radiod* under a debugger with most devices  
**users** Run *radiod* under a debugger with the Funcube dongle

And of course you need to be a member of the **sudo** group to execute programs as root.

Configuring *radiod*
--------------------

This is the most complex part of the setup, and I'm still writing documentation.
A quick-start guide still needs to be written, but you should start with the
examples in the **config** subdirectory. Although the list of available options is quite long,
most are rarely needed so most of the examples are actually fairly short and easy to understand.

The configuration options, including the rarely used esoteric ones,  are fully documented here:

[ka9q-radio.md](ka9q-radio.md) - Management by *systemd*; the [global] section of the *radiod* config file  
[ka9q-radio-2.md](ka9q-radio-2.md) - The hardware dependent section of the *radiod* config file  
[ka9q-radio-3.md](ka9q-radio-3.md) - Setting up static radio channels

Running *radiod*

(Move *systemd* control command descriptions from ka9q-radio.md to here.)

Optimizations
-------------

Read the file [FFTW3.md](FFTW3.md) on pre-computing efficient transforms for the FFTs in *radiod*.


