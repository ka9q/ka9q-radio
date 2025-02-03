Configuring and Running *ka9q-radio* - Part 1
============================================

v1.0 (in progress), August 2023  
Phil Karn, KA9Q
---------------

Introduction
------------

This is (or will be!) a complete reference to configuring and running
*ka9q-radio*. This is a powerful and flexible package, and "flexible"
often implies "complex".  Unfortunately this is also true for
*ka9q-radio* but I am working hard to make it easier given user
experience and feedback.

At the very least I will try to choose defaults carefully and to
provide canned configuration files for the most common use cases.

Please read the background information on *ka9q-radio* to understand
its general design. It is not meant to compete with the many other
SDRs with intuitive flashy GUIs, though one may get written for it
someday.  It's intended for applications that need many channels at
once. It would also make an excellent foundation for a web SDR able to
support many (hundreds) of simultaneous users. So far we've used it
for specialized applications like APRS gatewaying, repeater monitoring
and recording, multiband WSPR and FT8 skimming, propagation research,
and radiosonde reception that all use its multichannel capabilities.

The original version of *ka9q-radio* had a separate front end handler,
e.g., *funcubed* or *airspyd*, from the main radio daemon *radiod*.
In August 2023 I merged the front end handlers into *radiod*,
improving performance and simplifying configuration considerably. This
document only discusses this new version.

The *radiod* Daemon
-------------------

What follows assumes some knowledge of common Linux commands and
system administration, particularly configuring, starting and stopping
daemons running under the standard Linux system management daemon
*systemd*. *ka9q-radio* uses Linux conventions as much as possible.

The core of *ka9q-radio* is the radio daemon, *radiod*. Like all
daemons, *radiod* automatically runs in the background.  Users
talk to it only over the network with the client programs
*control* and *monitor* (if at all - most *ka9q-radio* applications
are completely automatic once configured).

Several instances of *radiod* may run at the same time, subject to
resource limits (USB, Ethernet and CPU capacity).  Front ends cannot
be shared between *radiod* instances, and each instance can only
handle one front end. 

Running "make install" in the *ka9q-radio* source
directory creates **/usr/local/sbin/radiod** and the *systemd*
service file **/etc/systemd/systemd/radiod@.service**.
*systemd* replaces the '@' character in a
running instance with the instance name. This file usually need not be modified.

*radiod* reads its
configuration from **/etc/radio/radiod@foo.conf**, where **foo** is the instance
name. You should pick a meaningful or descriptive instance name, e.g,
**hf**.  Note that **.conf** is *not* part of the
instance name; it's a common error to start "radiod@hf.conf" when you
mean "radiod@hf".

The main **systemctl** commands for controlling *radiod* (or any other
**systemd** service with multiple instances) are:

$ sudo systemctl start radiod@foo  
$ sudo systemctl stop radiod@foo  
$ sudo systemct restart radiod@foo  
$ sudo systemct enable radiod@foo  
$ sudo systemct disable radiod@foo  
$ systemctl status radiod@foo  

The first two commands immediately start and stop *radiod*. "systemctl restart"
is equivalent to a **systemctl stop** immediately followed by a
**systemctl start**. The **enable** and **disable** commands have no immediate effect;
they configure *systemd* to start *radiod* after a boot, or to
prevent that from happening.  It does this by creating or deleting a
symbolic link in
**/etc/systemd/system/multi-user.target.wants**. Again, this is
standard Linux stuff.

Like most system daemons, *radiod* writes startup and error messages
to the standard system log, */var/log/syslog*. You can read this
file directly (e.g., with **grep** or **tail**) or through the
**systemctl status** command.

You do not have to be root to run **systemctl status** but you usually
have to be root or a member of group "adm" to read /var/log/syslog.
Note that **systemctl status** only gives you the last 10 lines of output
from *radiod*.


The *radiod* Configuration File
-------------------------------

*radiod* configuration files are in "ini" format and contain at least
two parts and usually three or more.  The first part, always named
[global], sets parameters that apply to the entire *radiod* instance
plus any defaults to be applied to the channel sections. The second
part describes the SDR front end hardware to be used, and the
remaining sections describe one or more receiver channel groups to be
statically created at startup.

Undefined (or misspelled) entries in a section are simply ignored.
(I'd like to log them, but the *libiniparser* isn't set up to find them.)

The source comes with a collection of *radiod* configuration files you
may use directly or with slight modifications. I put a lot of thought into
defaults that would be suitable for the greatest number of situations, so only a few
entries are actually required (specifically **hardware** and **status**).
But just in case, here's a guide to what they all do.

The [global] Section
--------------------

This is the only reserved section name, and it must appear exactly once in
every *radiod* configuration file. Entries in the **[global]** section include:

### hardware = (no default, required)

Gives the name of the config file section that describes the
specific front end hardware to be used. It is usually, but need not
be, the same as the actual device type. This entry is required.

### status = (no default, required)

This gives the domain name of the multicast group that will be used
for status and control. The name will be registered with the Linux
multicast name server *avahi*. This entry is required.

This string is
deterministically hashed to generate and advertise an IPv4
multicast address in the site local 239.0.0.0/8 block, along with a SRV DNS
record of type _ka9q-ctl._udp with this name.

This parameter cannot specify a unicast IP address, though periodic
status transmissions are also sent on each active data channel every 500 ms,
and that can use a unicast destination.

### iface = (no default, optional)

Many computers, including most recent Raspberry Pis have
both a wired Ethernet interface (usually eth0) and a WiFi interface
(wlan0). If wlan0 comes up before eth0, which can happen if you have
a "smart" Ethernet switch with the spanning tree protocol (STP)
enabled, the Linux kernel may pick it as the default multicast
interface. Since most WiFi networks handle multicast traffic poorly,
this will probably result in lost traffic and erratic operation on
your entire network, not just for *ka9q-radio*.

This option gives the interface name of the local Ethernet device to
be used for all status/control and data traffic.  Don't set this
unless you really need to.

When **ttl = 0** (the default) the internal loopback interface is used automatically;
it need not be specified here

### dns = (optional, default off)

Set the global default for whether to use the Domain Name System (DNS) to resolve domain names in the *data* and *status*
fields.
The default (off) causes **radiod** to publish the specified name in the '.local' multicast DNS zone
along with an IPv4 multicast address in the 239.0.0.0/8 block generated from the
name by a deterministic hash.

Setting **dns = on** causes **radiod** to instead query
the domain name system for an existing name (by default, in the .local zone)
and use the resulting IP address. This is primarily intended
for sending an output stream to a regular (unicast) IP address, though the DNS entry
may point to a preset multicast address. IPv6 is not yet supported.

The **dns** parameter can be overridden in individual channel definitions.

### data = (no default, optional)

This sets the default domain name of the multicast group for receiver
output streams in any receiver channel group that doesn't explicitly
set one. A **data =** directive in a channel group overrides this one.

This option is required to create dynamic channels, otherwise it is optional
provided that it is specified in each channel group.

When the *dns* option (see above) is off, *radiod* deterministically hashes the destination string to generate
and advertise an IPv4 multicast address in the site local 239.0.0.0/8 block,
along with a SRV DNS record of type _rtp._udp or _opus._udp
advertising this name, depending on the output encoding. _rtp._udp is used for all PCM
formats.

When the *dns* option is on, the specified name is looked up in the Domain Name System,
and the corresponding address may be either a multicast or unicast IPv4 address.

A single multicast group can carry many receiver channels, each
distinguished by its 32-bit RTP SSRC (Real Time Protocol Stream Source
Identifier), which must be unique for an instance of
*radiod*. However, consider that Ethernet switches, routers and host
handle multicast group subscriptions by their IP addresses only, so an
application (e.g., *pcmrecord*) will discard traffic from
any unwanted SSRCs sharing an IP multicast address with desired
traffic. At a 24 kHz sample rate, each 16-bit mono PCM stream is 384
kb/s plus header overhead, so this can add up when many channels are
active.  This is usually OK on 1Gb/s Ethernet, but it can be a problem
over slower Ethernets or WiFi, especially where the base station does
not do multicast-to-unicast conversion. To minimize network bandwidth
when you're simply listening, use Opus output encoding.

### mode = (no default, optional)

Sets the default mode to be used for any channel group that doesn't
specify one. Modes are specified in the file
*/usr/local/share/ka9q-radio/modes.conf*. They would probably be
better called "presets" because a "mode" actually describes a group of
parameters (demodulator type, filter settings, etc) in that file.

This option is needed if you want to create dynamic channels, otherwise it is optional
provided that it is specified in each channel group.

### ttl = (optional, default 0)

Sets the Internet Protocol Time-to-Live (IP TTL) field in all
outbound traffic. The TTL field, or more accurately a "hop count limit"
field, limits how many hosts and routers may process the packet before
it is dropped. Multicast routers are rare, so in practice the usual
values are 0 and 1.

**ttl** *defaults to zero*, which keeps multicast traffic 
from even leaving the system that generates it. This avoids
swamping networks with low-end WiFi base stations and "dumb"
(unmanaged) Ethernet switches that flood multicast traffic to all
output ports. "Smart" switches that "snoop" IGMP to limit multicast
traffic to where it is wanted (and away from WiFi) are strongly recommended. **ttl = 0** is
fine as long as all applications that read status or data
from *radiod* run on the same physical system.

Note that *ttl* can only be set globally, not in individual demodulator sections.

### tos = (optional, default 48)

Sets the Internet Protocol Type-of-Service (TOS) field in all outbound traffic.
There is little reason to change this on a LAN with sufficient capacity.
This parameter also applies only globally.

### blocktime = (optional, default 20)

*ka9q-radio* filters with *fast convolution*;
this is how it efficiently demodulates hundreds of channels at once on
modest hardware. Fast convolution is based on the Fast Fourier
Transform (FFT), which processes incoming sample data in blocks. This
parameter sets that block size in milliseconds.  The default of 20 ms
(50 Hz) seems to be a good balance between efficiency, signal
I/O delay and kernel scheduling. I generally use only block times
supported by the Opus codec to ease transcoding the PCM output of
*radiod* to Opus by the *opusd* daemon; this is optional.

The actual block size in samples is equal to **blocktime** times the
A/D sample rate. E.g., the Airspy R2 has a 20 MHz (real) sample rate,
so a 20 ms **blocktime** corresponds to a block of 400,000 real
samples. The actual FFT block is larger because it includes data from
the previous block depending on the **overlap** setting.

Larger values of **blocktime** permit sharper channel filters and are
more tolerant to CPU scheduling variability but add latency and incur
greater CPU overhead. (The relative per-sample CPU cost of an FFT
increases with the logarithm of the FFT block size.)

### overlap = (optional, default 5)

Another fast convolution parameter that applies to the
entire *radiod* instance.  It specifies how much of each FFT block
is to overlap with the previous one. This is necessary to convert the
FFT's circular convolution into the linear convolution we need for
filtering.  It sets the fraction of each block that is old data from
the previous block.

An overlap of 5 (the default) means that 1/5 of each FFT input block
is from the previous block and 4/5 are new A/D samples. This limits
the duration of the impulse response of the individual channel
filters, with longer responses permitting sharper filter edges. For
example, an **overlap** of 5 combined with a **blocktime** of 20
milliseconds gives an overlap of 20 / 5 = 4 milliseconds.  Smaller
values of **overlap** permit sharper filters at the expense of greater
CPU loading since each FFT processes more old data.

Only certain values of **overlap** work properly, so stick with the
default unless you're feeling adventurous or until I write a more
detailed discussion. An **overlap** of 2 might be good for sharper
filters on HF, as the extra CPU load isn't a problem with lower A/D
sample rates.

### fft-threads = (optional, default 2)

Sets the number of FFT "worker" threads for the forward FFT shared by
all the receiver channels. The default is usually sufficient except on slow systems.
A single thread will suffice on fast CPUs, and may reduce overhead.

### rtcp = (optional, default off)

Enable the Real Time Protcol (RTP) Control protocol. Incomplete and
experimental; leave off for now.

### sap = (optional, default off)

Enable the Session Announcement Protocol (SAP). Eventually this will
make receiver streams visible to session browers in applications such
as VLC. Leave off for now.

### mode-file = (optional, default */usr/local/share/ka9q-radio/modes.conf*)

Specifies the mode description file mentioned in the **mode**
parameter above. Use the default when possible.

### wisdom-file = (optional, default */var/lib/ka9q-radio/wisdom*)

Specifies where FFTW3 should store accumulated "wisdom" information
about the fastest ways to perform *radiod*'s specific FFT transforms
on this specific CPU. Some of this is automatically computed when *radiod*
starts, but to avoid long startup times only minimal effort is expended.

FFTW3 also uses the "global wisdom" file */etc/fftw/wisdomf*, which
can be created with significantly more effort (and CPU time). Right
now I generate the latter file by hand with a fairly esoteric set of
commands, see [FFTW3.md](FFTW3.md). FFTW *can* generate this information
automatically when first run but can take *hours* to do so. I am
working on a better way, e.g., by automatically starting wisdom
generation in the background so that *radiod* starts immediately,
though of course it will run faster after wisdom generation is
complete and *radiod* is restarted to use it.

This document continues in [Part 2](ka9q-radio-2.md),
where the hardware definition section is described.




