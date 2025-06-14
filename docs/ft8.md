# Decoding FT4 and FT8 with ka9q-radio
With a wideband front end like the RX 888, *ka9q-radio* makes a very
effective multiband FT4/FT8 skimmer, simultanously decoding and
reporting everything to the crowdsourced spotting site
http://pskreporter.info.

You can help other hams by reporting what you hear, and possibly get
bragging rights. You should especially like seeing every band at a
glance if you're a contester or DXer.

This requires four elements:
- an instance of *ka9q-radio* receiving the FT4 and FT8 segments
- an instance of *ft8-record* and *ft4-record* to record them in 15 and 7.5 second "spool" files
- one or more instances of *ft8-decode* and *ft4-decode* to read and decode
the spool files
- instances of *pskreporter* to send your spots to http://pskreporter.info.

Because FT4 and FT8 use the same spooling, decoding and reporting
mechanisms, all references to ft8 in the following discussion also apply to
ft4; just substitute 'ft4' for 'ft8'.

The *pcmrecord* program invoked by the *ft8-record* service is part of *ka9q-radio*.

The *decode_ft8* program invoked by the *ft8-decode* service comes from
the separate git repository https://www.github.com/ka9q/ft8_lib, a
fork of https://www.github.com/kgoba/ft8_lib by Kārlis Goba with my
additions for processing spool directories. It handles both FT8 and FT4; FT8
is the default and the -4 option tells it to look for FT4.

The *pskreporter* program comes from the repository
https://www.github.com/pjsg/ftlib-pskreporter by Philip Gladstone
N1DO, written specifically for *ka9q-radio*. You will need to build
and install these two packages separately.

If you have several antennas and instances of *ka9q-radio* you can, if
you want, decode them all with a single instance of *ft8-record*,
*ft8-decode* and *pskreporter*.  This is an example of the versatility
of IP multicast. However, these three programs
communicate through files so they must run on the same system.  By
default, *ft8-record* writes its 15-second "audio" spool files to
**/var/lib/ka9q-radio/ft8/** and *ft8-decode* appends decoded spots to
**/var/log/ft8.log**.

Once everything is working, you can watch your spots roll in with
```
tail -f /var/log/ft8.log
```
I encourage others to find interesting uses for this data; N1DO's *pskreporter*
is just one possibility. I'd especially like to see a local map display
that doesn't rely on Internet connectivity.


## .service files

Each element of the FT8 spotting chain runs as a Linux *systemd* job
started by a **.service** file in **/etc/systemd/system/**. They are
automatically installed with `make install`
in **Makefile.linux** but must be enabled and started manually.
You should not have to modify them, but if you do
__beware__ that they may get overwritten by a subsequent run of `make
install`. The relevant files are

- **radiod&#64;.service**
- **ft8-record.service** (and **ft4-record.service**)
- **ft8-decode&#64;.service** (and **ft4-decode&#64;.service**)
- **pskreporter&#64;.service**

A '@' character in a **.service** file name means there can be (but
don't *have* to be) multiple instances of that service. However, you
must still specify an instance name even for a single instance.  There
is only one instance of *ft8-record* (plus one of *ft4-record*) that
records all of the receiver streams (bands) on a particular multicast
channel, which can be used for output by one or more *radiod* instances.  Each
instance of *ft8-decode* works on the same directory
processing files from all bands, so you can control parallelism by
how many you run. (FT8 and FT4 must use different spool directories
so the decoder knows which mode to look for.)
Multiple decoders use cooperative locking to ensure each file gets decoded and logged
once and then deleted.  The FT8 decoder is usually fast enough that just
one instance is enough to decode every band; I wrote
the spool handling code mainly to eventually apply it to WSPR
decoding, which is much more CPU intensive.

There is one instance of *pskreporter* for each mode, i.e.,
*pskreporter@ft8*, *pskreporter@ft4* or *pskreporter@wspr*. (WSPR is not
discussed in this document; see http://github.com/rrobinett/wsprdaemon.)

## Configuring *ft8-record* and *ft8-decode*

To avoid clobbering a local configuration, the `make install` command
in *ka9q-radio* never writes to **/etc/radio/**, so you must create and
manage them manually.  (I need a better way to handle this.)

The *ft8-record* and *ft8-decode* jobs share the same configuration
file **/etc/radio/ft8-decode.conf**, which should look like this:
```
MCAST=ft8.local
DIRECTORY=/var/lib/ka9q-radio/ft8
```
This should be fairly self-explanatory; **ft8.local** is the domain name of the
multicast group to which *radiod* sends its received streams, and
**/var/lib/ka9q-radio/ft8/** is the spool directory where *ft8-receive*
writes 15-second files of the form **20250614T134600Z_10130000_usb.wav**
for each band and *ft8-decode* then reads, decodes, logs and deletes them.
*ft8-decode* appends its decoded spots to **/var/log/ft8.log**, but this
can be changed in **ft8-decode&#64;.service**.

Any number of *radiod* channels, from one or more *radiod* instances,
may be sent to the same multicast group and they will all be handled.

However, if multiple *radiod* instances receive the same frequncies
(e.g., from different antennas), their spool files would ordinarily
have the same names. If this happens *ft8-record* appends a single
digit to the file name just before the **.wav** suffix to try to make
it unique.  Alternatively, **ft8-record.service** could be changed to
pass the `--prefix-source` option to the *pcmrecord* command to prefix
each file name with the IP address and port number of the source
computer, again making them unique.

The *pcmrecord* command used by *ft8-record* has long placed a set of
external file attributes on the files it creates.  The *decode_ft8*
command invoked by *ft8-decode* now uses two of them,
**user.unixstarttime** and **user.frequency**, to specify the starting
timestamp and base frequency.  Otherwise it falls back to extracting
them from the file name as before, so non-standard file names
can be used to resolve collisions as long as the file system
containing **/var/lib/ka9q-radio/ft8/** supports extended attributes. The
main exceptions are *vfat* and *tmpfs*, but most other modern Linux
filesystems like *XFS* and *ext4* do support them.

## Enabling and starting *ft8-record* and *ft8-decode*

This is very simple; just say
```
sudo enable ft8-record ft4-record ft8-decode@1 ft4-decode@1
sudo start ft8-record ft4-record ft8-decode@1 ft4-decode@1
```
Again, note that there's just one *ft4-record* and one *ft8-record* job so they
do not take a **@n** suffix. The *ft4-decode@* and *ft8-decode@* jobs *do*
require suffixes even if you only want one of each. 
If you want more, just add them like this:
```
sudo enable ft8-decode@2 ft8-decode@3 ...
sudo start ft8-decode@2 ft8-decode@3 ...
```
though this really shouldn't be necessary unless a large backlog
has built in a spool directory because *ft8-record* kept running
when all *ft8-decode@* jobs were stopped.

You can monitor the status of the record and decode jobs with the commands
```
systemctl status ft8-record	'ft8-decode@*'
```
You don't need sudo just to query status. Note the wildcard in the decode name;
systemd will expand it into a list of all currently running instances. The 'single quotes'
ensures that the shell passes the '*' to *systemctl* without expansion.

You can restart these jobs with
```
sudo systemctl try-restart ft8-record 'ft8-decode@*'
```
The ``try-restart`` command (as opposed to ``restart``) restarts only jobs that are already running; it helps
avoid unwanted instances should you mistype the names.

These programs log to the *systemd* journal and to **/var/log/syslog** (if the package
*rsyslog* is installed). You can examine these logs with, e.g,
```
journalctl -u 'ft8-decode@*'
```
or
```
grep ft8-record /var/log/syslog
```
Normally you should see little or nothing in the logs unless there's a problem.


## Configuring *ka9q-radio* to receive FT4 and FT8

You can use my own configuration files as a guide; they're
in **config/radiod&#64;ka9q-hf.conf.d/**. (I've split my *radiod* configuration
into separate directory files to make them
easier to manage. *radiod* concatenates them into a single temp file (order doesn't matter)
and reads them.
You can still use a unified **radiod&#64;name.conf** file if you like.) The file
**radiod&#64;ka9q-hf.conf.d/ft8.conf** lists every FT8
frequency range I know about. Note that most of the sample
rates and bandwidths are wider than 12 kHz and 3 kHz
respectively. Each takes in enough of a band to capture the DX
fox/hound segments that are usually just above the standard channels.
A file's sample rate is passed to the decoder through
the standard **.wav** file header; I have modified *decode_ft8* to handle them.
For example, on 20 meters I look for FT8 signals anywhere from 14.0711 MHz to 14.102 MHz,
and on 40 meters from 7.0561 MHz to 7.087 MHz:
```
[ft8-40-20m]
data = ft8.local
samprate = 64k
encoding = s16be
mode = usb
hang-time = 0
recovery-rate = 60
low = 100
high = 31k
freq = "7056k 14m071"
```
Normally you should configure *radiod* to produce 16-bit integer output streams
as here, but *decode_ft8* also accepts **encoding = f32le**.
**opus** and **f16le** encodings are *not* supported.

If you like, you could actually look for FT8 in an entire band; it
might be interesting to see what you find.

## Configuring *pskreporter*
To be written...

