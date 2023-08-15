*ka9q-radio* Installation Notes  
14 August 2023
===============================

The preferred platform is Debian Linux 12 ("bookworm") on the x86-64
and the 64-bit "bullseye" version of Raspberry Pi OS for the Raspberry
Pi 4. (Raspberry Pi OS is Debian Linux 11 with customizations. As of
this writing it has not yet incorporated Debian version 12.)  Older
versions may work, but you may have to fix some problems.

CPU Requirements
----------------

Most reasonably modern x86 systems can run *ka9q-radio* just
fine. Even an Intel i5 can handle the RX888 at its full sample rate of
130 MHz with plenty of time left over.  The bigger problem is just
getting the bits from the front end into the program over USB and in
ensuring that the front end has enough power. (And to the RPi, if
you're using one.) It's usually a good idea to dedicate a USB
controller to a front end to avoid contention with other devices. An
externally powered hub is often a good idea, especially on the RPi.

Be careful with other CPU-intensive tasks on the same system, even
at a high 'nice' level, unless you are running a realtime-enabled
version of the Linux kernel. *ka9q-radio* can use the Linux real-time
scheduling features only if the kernel supports them. (Debian provides
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

At the moment, *ka9q-radio* supports the following SDR front ends:

[Airspy R2/Airspy Mini](https://airspy.com/airspy-r2/)  
[Airspy HF+](https://airspy.com/airspy-hf-discovery/)  
[Generic RTL-SDR](https://en.wikipedia.org/wiki/Software-defined_radio#RTL-SDR) (tuner mode only)  
[AMSAT UK Funcube Pro+ Dongle](http://www.funcubedongle.com/)  
[RX-888 MkII](https://www.rtl-sdr.com/techminds-reviewing-the-rx888-mk2-software-defined-radio/)  (direct sampling mode only)

Until the recent appearance of the RX-888, my preferred SDRs (and the
ones I have the most experience with) were the Airspy R2 for VHF/UHF
and the Airspy HF+ for HF.  They are available at a moderate price and
work fairly well.

The new RX-888 MkII is rapidly becoming my SDR of choice because it
can direct sample at up to 130 Ms/s. With *ka9q-radio* it can receive
literally hundreds of simultaneous channels over all of LF, MF, HF and
lowband VHF (through 6m).  It does comes out of China and
documentation is somewhat lacking. Fortuntely, K4VZ, AI6VN and I have
made it work well on HF, where it is beginning to displace stacks of
KiwiSDRs for all-band WSPR monitoring.  [http://www.wsprdaemon.org/]

Some RX-888's have thermal problems especially at full sample rate
(129.6 MHz). Until they can be resolved I've set the default to half
rate (64.8 MHz).  Because the internal lowpass filter is fixed at 64
MHz, this may allow some lowband VHF signals to alias into the top of
HF. E.g., a local California Highway Patrol repeater on 39.8 MHz
aliases onto WWV at 25 MHz. You'll need an external 30 MHz low pass
filter.

The RX-888 has a 16-bit A/D. That much dynamic range is almost
overkill for HF radio, but proper gain setting is still
important. Right now you can manually set the analog gain and
attenuation, but there is as yet no software AGC as on the Airspy R2.

The Airspy R2 is still my VHF/UHF workhorse, but this may change soon.
The RX-888 MkII includes a VHF/UHF tuner that *should* be able to
functionally replace the Airspy R2 for roughly the same price if you
get it from China. (It's available through Amazon but for a higher
price.)  I will support the RX-888's VHF/UHF tuner, but at present I
only support its direct sample (LF/MF/HF) mode. For VHF/UHF the RX-888
uses the same type of DVB-T (digital television) tuner as the Airspy
R2, so it will be limited to the same ~9 MHz or so of bandwidth; there
will be no benefit to its high sample rate in that mode.

The Airspy Mini looks just like an Airspy R2 to software, so I also
support it.  Although it advertises a lower sample rate, you can force
it to 20 Ms/s (real), same as the Airspy R2. However it's in a smaller
package that gets very hot, so I suspect that's why the rated sample
rate is lower.  I wouldn't push it without adequate heat dissipation.

The Airspy HF+ still works well with a good built-in AGC and dynamic
range. But it has a maximum sample rate of 912 ks/s (complex) so it can
only cover one HF band at a time, tops.  Our focus is definitely
moving to the RX-888 because of its killer ability to moniter all of
HF (and more) at once.

I began this project with the AMSAT UK Funcube dongle, but it's long
obsolete.  Don't bother unless you already have some on hand or can
get them cheap or free. Although originally designed for VHF/UHF
satellite reception, it also has good HF coverage and reasonably good
front end preselectors.  But is severely limited by its 192 ks/s
(complex) sample rate. That may be OK for some specialized applications
ike the receive-only APRS i-Gate I've been running for years on a RPi3.
The low sample rate demands minimal CPU.

The RTL-SDR is very popular because of its very low price, and while I
support it in *ka9q-radio* I haven't actually used it much myself. Its
main drawback is its narrow 8-bit A/D and limited dynamic range. Most
of my front end handlers have (optional) software AGC but there's
still no substitute for dynamic range, especially if you have strong
intermittent signals.

Finally, a word about the SDRPlay. I bought one many years ago before
I discovered that its libraries are proprietary and available only as
compiled binaries. I consider this unacceptable, especially since its
competitors all provide open source libraries in the standard Linux
distributions. So my SDRPlay has been gathering dust on my shelf. I
don't feel particularly inspired to support a product whose vendor
goes out of their way to make it so difficult. If someone can provide
an open-source substitute for the proprietary SDRPlay library, I'll be
happy to reconsider supporting it.


Front end drivers now merged into *radiod*
------------------------------------------

*ka9q-radio* was substantially restructured during the summer of 2023 to merge the front end
drivers into *radiod*. The drivers are no longer separate programs using multicast IP to communicate
with *radiod*. This considerably improves performance and simplifies configuration; what used to be a separate config file
(e.g., */etc/radio/airspyd.conf*) is now a section in the *radiod* configuration file. The separate drivers
(*airspyd*, etc) are gone.

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

Linux multicast routing is still a mystery to me. With hosts having just one Ethernet interface it usually "just works"; multicast traffic goes to the local Ethernet LAN.
But I sometimes have problems on systems with multiple interfaces
(e.g, an Ethernet and WiFi).
My RPis run headless so I usually have both Ethernet and WiFi enabled to ensure that I can get in remotely. Sometimes the WiFi comes up before the Ethernet due to the
STP forwarding delay in my smart switches. Systemd starts daemons (like radiod) so quickly that if it comes up during this period,
Linux will route its multicast traffic to WiFi. And it will stay this way even after the Ethernet comes up. This is definitely not what we want.

Sometimes just restarting radiod will fix the problem, but this is messy.

I haven't found a simple, automatic fix to this problem yet, so I added the **iface** option
to the [global] section of the config file. Say something like **iface**=eth0.
Problem is, thanks to the brain dead and ridiculously misnamed "predictable interface names" misfeature in some
versions of Linux the name of the primary Ethernet interface is *not* always "eth0", but something cryptic like "enp0s3".
So if your primary Ethernet interface isn't "eth0", either remove the **iface** option or make sure the **iface** option specifies it.



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








