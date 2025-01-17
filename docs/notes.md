*ka9q-radio* Miscellaneous Installation Notes  
16 January 2025
===============================

Software Platform
-----------------

The preferred platform is Debian Linux 12 ("bookworm") on the x86-64
and Raspberry Pi 4.  Older versions may work, but you may have to fix
some problems.

Because I have a Macbook running MacOS, I regularly compile
*ka9q-radio* on it and run *monitor* and *control*, the user
interface programs.  This requires installing a bunch of
[MacPorts](https://www.macports.org/) packages that give me a workable
imitation of a Linux environment.  I'll document this process
eventually. I have not actually run *radiod* itself under MacOS; I
target only Debian Linux. I'm not particularly interested in porting *ka9q-radio*
to a wide range of environments, at least not yet. And I don't think I'll
*ever* want to bother with Microsoft Windows!


CPU Requirements
----------------

Most reasonably modern x86 Linux systems can run *ka9q-radio* just
fine. Even an Intel i5 can handle the RX888 at its full sample rate of
130 MHz with plenty of time left over.  The bigger problem is just
getting the bits from the front end over USB and into *radiod* and
ensuring that the front end has enough power. (And the RPi, if
you're using one.) To avoid contention, it's usually a good idea to dedicate a USB
controller to a front end.
An externally powered hub is often a good idea, especially on the RPi.

Be careful with other CPU-intensive tasks on the same system, even
when 'niced', unless you are running a realtime-enabled
version of the Linux kernel. *ka9q-radio* will use Linux real-time
scheduling features, but only if the kernel supports them. (Debian provides
both realtime and standard kernels for each release). The RX888 works
fine at 64 Ms/s on the Orange Pi **until** you try to run CPU soakers;
then *ka9q-radio* begins to badly lose data. AI6VN and I believe this
is because the standard OPi kernel is not real-time enabled, and we
can't find and install one that is.

The Raspberry Pi 4 is sufficient to handle the Airspy R2 (20 Ms/s
real) even with several hundred channels, but it is *not* fast enough
for the RX888 except at low sample rates.

Supported Hardware and System Requirements
------------------------------------------

*ka9q-radio* currently supports the following SDR front ends:

[Airspy R2/Airspy Mini](https://airspy.com/airspy-r2/)  
[Airspy HF+](https://airspy.com/airspy-hf-discovery/)  
[Generic RTL-SDR](https://en.wikipedia.org/wiki/Software-defined_radio#RTL-SDR) (tuner mode only)  
[AMSAT UK Funcube Pro+ Dongle](http://www.funcubedongle.com/)  
[RX-888 MkII](https://www.rtl-sdr.com/techminds-reviewing-the-rx888-mk2-software-defined-radio/)  (direct sampling mode only)
[SDRPlay](https://www.sdrplay.com)__
[Fobos](https://rigexpert.com/software-defined-radio-sdr/fobos-sdr/#)

Until recently my preferred SDRs (and the ones I still have the most
experience with) were the Airspy R2 for VHF/UHF and the Airspy HF+ for
HF.  They are available at a moderate price and work fairly well. The
Airspy R2 samples at 20 Ms/s (real), covers almost 9 MHz at once, and
is still my VHF/UHF workhorse. This may change soon in favor of
the RX-888 (see below).

The Airspy Mini looks just like an Airspy R2 to software, so it's also
supported.  Although it advertises a lower sample rate (12 Ms/s real)
you can force it to the 20 Ms/s (real) rate of the Airspy R2. But it's
in a smaller package that gets very hot, so that's probably why the
specified sample rate is lower.  Don't push it without adequate
heat dissipation. Also, it seems to have more spurs than the R2.

The Airspy HF+ works well, with a good built-in AGC and
wide dynamic range. But it has a maximum sample rate of 912 ks/s
(complex) so it can only cover one HF band at a time.  Our focus
has been on the RX-888 MkII because of its killer ability to
monitor all of HF (and more) at once.

The new RX-888 MkII can direct sample at up to 130 Ms/s. With
*ka9q-radio* it can simultaneously receive hundreds of channels over
all of LF, MF, HF and lowband VHF (through 6m). The main drawback? It
comes out of China and documentation is sparse. Fortunately, K4VZ,
AI6VN and I have it working well on HF, where it is beginning to
displace stacks of KiwiSDRs for all-band WSPR monitoring.
[http://www.wsprdaemon.org/]

Some RX-888's have thermal problems especially at full sample rate
(129.6 MHz). Until they can be resolved I've set the default to half
rate (64.8 MHz); you can still override this.
Because the internal lowpass filter is fixed at 64
MHz, any lowband VHF signals will alias onto upper
HF. E.g., a California Highway Patrol repeater near me on 39.8 MHz
aliases onto WWV at 25 MHz. You'll need an external 30 MHz low pass
filter (or two).

The RX-888 has a LTC2208 16-bit A/D. That much dynamic range (about
100 dB) is probably overkill for HF radio because the thermal noise
floor of the AD8370 variable gain amplifier swamps the A/D quantizaton
noise. But proper gain setting is still important to minimize the
overall noise figure. While background noise is so high on MF and
lower HF that receiver noise figure (NF) is almost irrelevant, it's
much more important at high HF where background noise is much
lower. If you increase gain for good sensitivity on the high end, you
may be easily overdriven by AM broadcast stations. An AM blocking
filter will help, but the real answer is a "shelving filter" shaped to
the entire spectrum seen by the RX-888. Paul, WB6CXC, makes a combined
HF shelving and anti-alias filter specifically for this application and it works well. 

The RX-888 MkII includes a VHF/UHF tuner that *should* be able to
functionally replace the Airspy R2, but I don't support it yet; right
now the RX-888 is direct-sampling (HF) mode only.  For VHF/UHF the
RX-888 uses the same type of DVB-T (digital television) tuner as the
Airspy R2, so it will be limited to the same ~9 MHz or so of
bandwidth; there will be no point in running it at full sample rate in
that mode.

I began this project with the AMSAT UK Funcube dongle, but it's long
obsolete.  Don't bother unless you already have some on hand or can
get them cheap or free. Although originally designed for VHF/UHF
satellite reception, it also has good HF coverage and reasonably good
front end preselectors.  But is severely limited by its 192 ks/s
(complex) sample rate. That may be OK for some specialized applications
like the receive-only APRS i-Gate I've been running for years on a RPi3.
The low sample rate demands minimal CPU.

The RTL-SDR is very popular because it's cheap, and while I
support it in *ka9q-radio* I haven't actually used it much myself. Its
main drawback is a narrow 8-bit A/D and limited dynamic range. Most
of my front end handlers have (optional) software AGC but there's
still no substitute for sufficient dynamic range, especially if you have strong
intermittent signals.

I have a HackRF that I once used with *ka9q-radio* but I set it
aside when I got the Airspy R2. When I find time I will dust it off
and re-integrate it into the current package.

The Fobos wideband receiver from Rigexpert is newly supported, though
I don't actually have one. The required low-level driver library is
not yet a standard Debian package you can install with 'apt get', but
you can build it from the sources fairly easily. The git repository is
at http://github.com/rigexpert/libfobos. You build it with the sequence
mkdir build; cd build; cmake ..; make; sudo make install. You may need to install the 'cmake' command.

Finally, a word about the SDRPlay. I bought one many years ago only to
discover that its libraries are proprietary and available only as
compiled binaries. It sat on my shelf for years because I consider
this unacceptable, especially since its competitors all provide open
source libraries in the standard Linux distributions.

But the SDRPlay is finally now supported using the new (January 2025)
dynamic driver loading feature in *ka9q-radio*.  You must first
download and install their proprietary driver. Don't bother with the
sdrplay.com website, it's an excercise in frustration. I found
http://github.com/srcejon/sdrplayapi and it works.
It does install a closed binary blob called "sdrplay_apiService" that it runs with *systemd*.
I have no idea what it does, but it burns almost as much CPU as *radiod*.

Front end drivers can now be dynamically loaded
-----------------------------------------------

As of January 2025, *ka9q-radio* can load hardware drivers dynamically
using the Linux/UNIX shared library facility; previously all drivers
had to be statically linked into the *radiod* binary.  The existing
drivers for the rx888, airspy, airspyhf+, rtlsdr, funcube and sig_gen
are still statically linked into *radio* but this can be overridden by
specifying DYNAMIC=1 to 'make' to force them to be built dynamically
as well.  The shared libraries are installed in
/var/local/lib/ka9q-radio.  By default, only those devices for which a
Debian library package already are automatically built when
*ka9q-radio* is installed; this excludes Fobos and SDRPlay because
they require libraries from third party sources that must be manually
built and installed.  If you have them installed, you can build
*ka9q-radio* with "make FOBOS=1" or "make SDRPLAY=1" (or both) as
appropriate.

Here's an incomplete list of nits and gotchas I've run into while
installing *ka9q-radio* on various systems.

Set the Locale
--------------

The *control* program wants to display frequencies and other large
numbers with commas between groups of 3 digits for readability, and it
assumes it is doing so when moving the cursor around in the "Tuning"
window.  This won't work right if the default "C" locale is in use. To
fix this, use the appropriate locale for your site; e.g., for the USA,
use "en_US.UTF-8". To set the locale on the Raspberry Pi, run
*raspi-config*, select "Localisation [sic] Options", then "Locale",
and mark the appropriate locale(s). Be sure to set the system default on
the next screen; C.UTF-8 will *not* work.

On a generic Linux system you can set your locale with the
shell environment variable "LANG", e.g.:

>export LANG=en_US.UTF-8

Multicast Interfaces And Routing
--------------------------------

Linux multicast routing is still a mystery to me. On hosts with just
one Ethernet interface it usually "just works"; multicast traffic
automatically goes to the local Ethernet LAN.  But I sometimes have
problems on systems with multiple interfaces (e.g, an Ethernet and
WiFi).  Sometimes the WiFi comes up before the Ethernet due to the
spanning tree protocol (STP) forwarding delay in my smart
switches. After a reboot, Linux *systemd* may start *radiod* so
quickly that the kernel will route *radiod*'s multicast traffic to
WiFi and keep doing so even after the Ethernet comes up. This is
definitely not what we want.

Sometimes just restarting radiod will fix the problem, but that's messy.

I haven't found a simple, automatic fix to this problem yet, so I
added the **iface** option to the [global] section of the config
file. Say something like **iface**=eth0.  Problem is, thanks to the
brain dead and ridiculously misnamed "predictable interface names"
misfeature in some versions of Linux the name of the primary Ethernet
interface is *not* always "eth0", but something cryptic (and
unpredictable) like "enp0s3".  So if your primary Ethernet interface
isn't "eth0", either remove the **iface** option entirely or make sure
the **iface** option specifies it.  I'm still looking for solutions to
this problem, so please let me know if you have any insights.


Multicast DNS problems on Ubuntu
--------------------------------

Multicast DNS, which *radiod* relies on, is rather badly broken on
some older versions of Ubuntu, such as 20.04 ("focal"). The symptom
are messages like

>avahi service '2m vertical (2m-vertical-data.local)' successfully established.  
>resolve_mcast getaddrinfo(2m-vertical-data.local,(null)): Temporary failure in name resolution  
>resolve_mcast getaddrinfo(2m-vertical-data.local,(null)): Temporary failure in name resolution

I.e., multicast DNS lookups repeatedly fail even after the name is successfully registered with *avahi*.

The problem is that the "mdns4_minimal" resolver specified in
/etc/nsswitch.conf ignores multicast addresses (this seems to have
been fixed in later versions). To work around this, edit
*/etc/nsswitch.conf* to change "mdns4_minimal" to "mdns4". Then create
the file */etc/mdns.allow* with the entries

>>.local  
>>.local.

so it will only try to resolve names in the mDNS zone ".local", which
is what mdns4_minimal normally does.








