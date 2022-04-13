This package is designed for Debian Linux, including the Raspberry Pi
OS. Since I use a Macbook Pro as my desktop, some of it (e.g., the
interactive components 'control' and 'monitor') will also compile and
run on MacOS -- but not all of it. However, I'm interested in fixing
any unnecessary non-portabilities.

Prerequisites

Building this package on Debian requires the following packages be installed with 'apt install':

libusb-1.0-0-dev
libncurses5-dev
libudev-dev
libfftw3-dev
libattr1-dev
libbsd-dev
libhackrf-dev
libopus-dev
libairspy-dev
libairspyhf-dev
librtlsdr-dev
libiniparser-dev
libavahi-client-dev
portaudio19-dev
libopus-dev

Then:

$ ln -s Makefile.linux Makefile
$ make
$ sudo make install

This will write into the following directories:

/usr/local/sbin	     	 	   daemon binaries (e.g., 'radio')
/usr/local/bin		 	   application programs (e.g., 'control')
/usr/local/share/ka9q-radio	   support files (e.g., 'modes.conf')
/var/lib/ka9q-radio		   application state files (e.g., tune-*)
/etc/systemd/system - 		   systemd unit files (e.g., radio@.service)
/etc/sysctl.d	    		   system configuration files (e.g., 98-sockbuf.conf)
/etc/udev/rules.d		   device daemon rule files (e.g., 52-airspy.rules)
/etc/fftw			   FFTW "wisdom" files (i.e., wisdomf)
/etc/radio			   program config files (e.g., radio@2m.conf)

It will also create several special system users and groups.

Read the file FFTW3.md on pre-computing efficient transforms for the FFTs in 'radio'.
