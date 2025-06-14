# Decoding FT4 and FT8 wih ka9q-radio
With a wideband front end like the RX 888, ka9q-radio makes a very
effective multiband FT4/FT8 skimmer, receiving and decoding every segment
on every band simultaneously and optionally reporting spots to the crowdsourced
spotting site *pskreporter.info*

Not only can you help other hams by reporting what you can hear (and
getting bragging rights if you report a lot), but seeing all the bands
at a glance should be expecially useful if you're a contester
or DXer.

This requires four elements: an instance of *ka9q-radio* receiving the
FT4 and FT8 segments; an instance of *ft8-record* and *ft4-record* to
record them in 15 and 7.5 second "spool" files; one
or more instances of *ft8-decode* and *ft4-decode* to read and decode
the spool files; and instances of *pskreporter* to send your spots to
*pskreporter.info*.

Because FT4 and FT8 use the same spooling, decoding and reporting
mechanisms, all references to ft8 in this discussion apply equally to
ft4; just substitue 'ft4' for 'ft8' in the names.

The *decode_ft8* program invoked by *ft8-decode@.service* comes from
the separate git repository *ka9q/ft8_lib*, a fork of *kgoba/ft8_lib*
by Kārlis Goba with my additions for processing spool directories. The
*pskreporter* program comes from the repository
*pjsg/ftlib-pskreporter* by Philip Gladstone N1DO, written
specifically for *ka9q-radio*. You will need to build and install
these two packages separately.

If you have more than one antenna and instance of *ka9q-radio* you
can, if you want, easily decode digital segments from all of them with
a single instance of *ft8-record*, *ft8-decode* and *pskreporter*.
This is an example of the versatility of IP multicast. However, the
*ft8-record*, *ft8-decode* and *pskreporter* instances must run on the
same system since they communicate through shared disk files,
specifically */var/lib/ka9q-radio/ft8* for the 15-second "audio" spool
files and */var/log/ft8.log* for decoded spots.

A very simple way to see that everything is working is to execute
```
tail -f /var/log/ft8.log
```
I encourage others to find interesting uses for this data; N1DO's *pskreporter*
is just one possibility. I'd especially like to see a local map display
that doesn't rely on Internet connectivity.


## .service Files

Each element of the FT8 spotting chain runs as a Linux *systemd* job
started by a service file in */etc/systemd/system*. They are
automatically installed but not enabled or started by "make install"
in Makefile.linux.  You should not have to modify them, but if you do
**beware** that they may get overwritten by a subsequent run of "make
install". The relevant files are

- radiod@.service
- ft8-record.service (and ft4-record.service)
- ft8-decode@.service (and ft4-decode@.service)
- pskreporter@.service

The '@' character in a .service file name means there can be multiple
instances of that service, though this is not mandatory. However, you
still need to specify an instance name even for a single instance.
There is only one instance each of *ft8-record* and *ft4-record* that
records all of the receiver streams (bands) on a particular multicast
channel. Each instance of *ft8-decode* and *ft4-decode* works on the
entire spool, processing files from all bands, so you can get some
parallelism by starting more.  Multiple decoders cooperate with each
other, locking spool files so each gets decoded once and deleted.  The
FT8 decoder is usually fast enough that you need only one instance to
decode all the FT8 traffic on all the bands; I wrote the spool
handling code mainly to eventually apply it to WSPR decoding, which is
much more CPU intensive.

## Configuring ft8-record and ft8-decode

To prevent accidental overites of local configurations, the "make
install" command in *ka9q-radio* never writes to */etc/radio*, so you
must create and manage them manually.  (I need a better way to handle this.)

The *ft8-record* and *ft8-decode* jobs share the same configuration file
*/etc/radio/ft8-decode.conf*, which should look like this:
```
MCAST=ft8.local
DIRECTORY=/var/lib/ka9q-radio/ft8
```
This should be fairly self-explanatory; ft8.local is the domain
name of the multicast group to which *radiod* sends its received streams,
and /var/lib/ka9q-radio/ft8 is the spool directory where *ft8-receive*
records 15-second *.wav* audio files for each band and *ft8-decode* decodes
and deletes them. Any number of receive channels may share the same multicast
group and *ft8-record* will handle them all.

The output of ft8-decode is currently fixed to be /var/log/ft8.log, but this
can be changed in the ft8-decode@.service file.

Things get a litle complicated if you have multiple instances of
*radiod* receiving the same bands, e.g., from different antennas, as
they will generate spool files with the same name. This is not fully
handled yet, although in the event of a file name collision
*ft8-record* now appends a single digit to the file name just before
the *.wav* suffix to make it unique.  Alternatively, the *pcmrecord*
command used by the *ft8-record* job supports the --prefix-source
option to prefix each recorded file name with the IP address and port
number of the source, again making it unique..

The *decode_ft8* command invoked by *ft8-decode* now gets the start
time and base frequency through external file attributes placed on the
spool file by the *pcmrecord* program ("user.unixstarttime" and
"user.frequency", respectively. *decode_ft* tries to extract them from
the file name only when these attributes aren't present, so
non-standard file names should be okay as long as the file system
containing */var/lib/ka9q-radio/ft8* supports them. The main
exceptions are *vfat* and *tmpfs* so don't use them for the spool. Use
a regular Linux file system; all the modern ones (eg, XFS, ext4)
support them.

## Enabling and starting ft8-record and ft8-decode

This is very simple; just say
```
sudo enable ft8-record ft4-record ft8-decode@1 ft4-decode
sudo start ft8-record ft4-record ft8-decode@1 ft4-decode
```
If you want multiple instances of the ft4-decoder, just add them to these lines, e.g,
```
sudo enable ft8-decode@2 ft8-decode@3 ...
sudo start ft8-decode@2 ft8-decode@3 ...
```
though this really shouldn't be necessary unless the decoders had been
stopped while the recorders continued, long enough to build up a
large backlog in the spool directories.

## Configuring *ka9q-radio* to receive FT4 and FT8

You can use my own configuration files as a guide; they're available
in *config/radiod@ka9q-hf.conf.d*. I have implemented the option to
split the radiod config file into sections in a directory to make them
easier to manage. The file ft8.conf in that directory lists the HF FT8
channels I am currently receiving. Note that most of the sampling
rates and bandwidths are larger than the usual 12 kHz and 3 kHz
respectively; this takes in enough of each band to capture the DX
fox/hound segments that are usually above the standard ranges.
The sample rates are passed to the decoder through the sample rate 
field in the *.wav* file header; I have modified *decode_ft8* to handle them.
For example, on 20 meters I look for FT8 signals anywhere from 14.0711 MHz to 14.102 MHz,
and on 40 meters from 7.0561 to 7.087 MHz:
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

If you like, you could actually look for FT8 in an entire band;
it might be interesting to see what you find.

## Configuring *pskreporter*
To be written...

