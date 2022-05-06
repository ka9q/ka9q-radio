Configuring *ka9q-radio* to use the Airspy SDRs  
v1.0, May 2022  
Phil Karn, KA9Q
==============================

The configuration file */etc/radio/airspyd.conf* (and/or any files
under */etc/radio/airspyd.conf.d*) specify your Airspy R2, Airspy Mini
and Airspy HF+ SDR front end devices to the *ka9q-radio*
package. The Airspy R2 and Airspy Mini use the *airspyd* daemon and
the Airspy HF+ uses *airspyhfd* but all device types and both
daemons use the same configuration file(s).

They are in 'ini' format with one section for each device. Here's
one from my own station, which you will need to edit for your use:

>[2m]  
description = "2m vertical"  
serial = 91d064dc27839fcf  
iface = eth0	           ; force primary interface, avoid wifi  
status = 2m-vertical.local  
data = 2m-vertical-data.local  
linearity = 1              ; default is off  

The section name [2m] will become the name of the instance of *airspyd*
(or *airspyhfd*) handling it. Each section describes one
Airspy device; you can have as many as you want, limited only by
your system USB capacity. (The Airspy R2 generates 240 Mb/s, but
it's a USB 2.0 -- not 3.0 -- device so each will have to be on its
own USB host controller.)

As with any Linux system daemon, once configured you may enable it
to start automatically on the next reboot with

>$ `sudo systemctl enable airspyd@2m`

You may also start (or stop, or restart) it immediately with the
commands

>$ `sudo systemctl start airspyd@2m`

>$ `sudo systemctl stop airspyd@2m`

>$ `sudo systemctl restart airspyd@2m`

These manipulate the Linux *systemd* service unit files
*/etc/systemd/system/airspyd@.service* and
*/etc/systemd/system/airspyhfd@.service*. By default *systemd* will try
to restart a failed daemon every 5 seconds until manually
stopped (i.e., with *systemctl stop*). For this reason the daemons
themselves generally exit on error (e.g, if the device is unplugged
from the USB) rather than attempt to recover.

Use the *systemctl status* command to display the status of an *airspyd* or
*airspyhfd* instance:

>$ `systemctl status airspyd@2m`  
● airspyd@2m.service - Airspy-2m daemon  
Loaded: loaded (/etc/systemd/system/airspyd@.service; enabled; vendor preset: enabled)  
Active: active (running) since Mon 2022-05-02 18:01:20 PDT; 53s ago  
Main PID: 304581 (airspyd)  
Tasks: 7 (limit: 38341)  
CPU: 3.119s  
CGroup: /system.slice/system-airspyd.slice/airspyd@2m.service  
└─304581 /usr/local/sbin/airspyd 2m  

May 02 18:01:20 brian.ka9q.net airspyd[304581]: Using config file /etc/radio/airspyd.conf  
May 02 18:01:20 brian.ka9q.net airspyd[304581]: Airspy serial 91d064dc27839fcf, hw version AirSpy NOS v1.0.0-rc10-6-g4008185 2020-05-08, library version 1.0.9  
May 02 18:01:20 brian.ka9q.net airspyd[304581]: 2 sample rates: 20,000,000 5,000,000  
May 02 18:01:20 brian.ka9q.net airspyd[304581]: Set sample rate 20,000,000 Hz, offset 5,000,000 Hz  
May 02 18:01:20 brian.ka9q.net airspyd[304581]: Software AGC 1; LNA AGC 0, Mix AGC 0, LNA gain 14, Mix gain 12, VGA gain 13, gainstep 21, bias tee 0  
May 02 18:01:20 brian.ka9q.net airspyd[304581]: Status TTL 1, Data TTL 0, blocksize 32,768 samples, 49,152 bytes  
May 02 18:01:21 brian.ka9q.net airspyd[304581]: avahi service '2m vertical (2m-vertical.local)' successfully established.  
May 02 18:01:21 brian.ka9q.net airspyd[304581]: avahi service '2m vertical (2m-vertical-data.local)' successfully established.  
May 02 18:01:21 brian.ka9q.net airspyd[304581]: Using tuner state file /var/lib/ka9q-radio//tune-airspy.91d064dc27839fcf  
May 02 18:01:21 brian.ka9q.net airspyd[304581]: Setting initial frequency 153,391,009.569 Hz, not locked  

The supported configuration keys in each section are as follows:

**description** Optional. Gives free-format text that
will be carried through the *radiod*  program to the
control/status stream and the *control* program that
listens to it. It will also be advertised in DNS SRV (service
discovery) records by the Linux mDNS daemon *avahi*, so keep
it short but descriptive.

**serial**  Required. This must exactly match the
Airspy R2 64-bit serial number, in hex (the leading 0x is optional).
This can be read with the *airspy_info*  utility in the *airspy*
Debian package:

>$ `airspy_info`  
airspy_lib_version: 1.0.9  
Found AirSpy board 1  
Board ID Number: 0 (AIRSPY)  
Firmware Version: AirSpy NOS v1.0.0-rc10-6-g4008185 2020-05-08  
Part ID Number: 0x6906002B 0x00000030  
Serial Number: 0x91D064DC27839FCF  
Supported sample rates:  
    10.000000 MSPS  
    2.500000 MSPS  
Close board 1
   
Note that *airspy_info* will not see the device when any other
program (including *airspyd*) has it open. Any Airspy devices with
serial numbers not in *airspyd.conf* (or in a file under
*airspyd.conf.d*) are ignored.  If an *airspyd* or *airspyhfd*
instance is started for a non-existent device it will exit and Linux
*systemd* will restart it every 5 seconds until the device appears.

I find it very helpful to label each of my Airspy devices with
their serial numbers.

**iface**  Optional, but recommended. This specifies the
network interface to be used for all multicast network traffic from
*airspyd*  or *airspyhfd*. If not specified, the default
Linux multicast interface will be used. *This may not be what you
want!* Many computers, including most recent Raspberry Pis have
both a wired Ethernet interface (usually eth0) and a WiFi interface
(wlan0). If wlan0 comes up before eth0, which can happen if you have
a "smart" Ethernet switch with the spanning tree protocol (STP)
enabled, the Linux kernel may pick it as the default multicast
interface. Since most WiFi networks handle multicast traffic poorly,
this will probably result in lost traffic and erratic operation on
your entire network, not just for *ka9q-radio*.

**status**  The status name is passed to *radiod* 
(the *ka9q-radio*  downconverter/demodulator daemon) through
its own config file (e.g., */etc/radio/radio@foo.conf*) to use the
desired front end. Note that the names of the *radiod*  and *airspyd* 
or *airspyhfd* instances need not be the same.

**data** Required. This specifies the domain name
of the multicast group for the raw A/D sample stream. As with **status**
the default suffix is ".local" and an IPv4 address in the
239.0.0.0/8 block will be generated by hashing. Because this address
is included in the status stream it does not need to be manually
specified to *radiod* . A DNS PTR record of type _rtp._udp will
be advertised.

**linearity** Boolean, default off. Like most second-generation SDRs
with analog tuners, the Airspy R2 has three stages of analog gain
ahead of the A/D converters that any AGC must carefully manage. The
Airspy library provides separate gain tables optimized for sensitivity
and for linearity (i.e. resistance to intermod). *airspyd* uses the
sensitivity table by default, but in areas with strong signals the
linearity table may provide better resistance to intermod. I'm about 6
km line of sight from a dozen FM broadcast transmitters so I often use the
linearity setting.

Other settings
--------------

More settings are available but the defaults are sufficient for most
situations. Several are taken directly from calls in the Airspy and
Airspy HF+ driver libraries with little documentation, so I don't
actually know what they do. (Any details would be welcome.)

**blocksize** Defaults for *airspyd*: 32,768 if **data-ttl**
= 0, 960 otherwise; defaults for *airspyhfd*: 2,048 if **data-ttl**
= 0, 128 otherwise.

Set the number of samples in each data IP packet. The defaults depend
on **data-ttl** because the 1500 byte Ethernet MTU (Maximum
Transmission Unit, aka packet size limit) is relevant only for traffic
that actually has to pass through the hardware, and larger packets (up
to the 64KB IPv4 datagram limit) are much more efficient when looped
back in software. The Airspy R2 produces packed 12 bit real integers,
so 960 samples occupies 1,440 bytes, leaving room for the IP, UDP and
RTP headers. When **data-ttl**=0, the 32,768 samples occupy 49,152 bytes,
comfortably within the 64KB limit while cleanly dividing (by 3) the
98,304 sample callback buffer inside the Airspy R2 library driver to
keep the IP packets the same size.

The Airspy HF+ produces 8-byte complex floating point samples.  The
library callback buffer contains 2,048 samples (16,384 bytes) and this
is used when **data-ttl** = 0. The default blocksize is reduced to only
128 samples (1,024 bytes) when **data-ttl** > 0 to still cleanly divide
the callback buffer (by 16).

**data-ttl** Default 0. Sets the Internet Protocol Time-to-Live field,
more accurately described as a hop count limit. This limits the number
of IP routers through which the A/D data packets may be routed even to
hosts actively subscribing to it. Setting **data-ttl** = 0 keeps the
data traffic from leaving the host running *airspyd* or *airspyhfd*
but it can still be received by any program (such as *radiod*) running on
the same host.

The default is 0 because the A/D stream is a *lot* of traffic (240
Mb/s for the Airspy R2) and *radiod* running on the same computer is
usually its only consumer. This also sets the default packet size (see
**blocksize**) to a much larger and much more CPU-efficient value than
would be necessary to avoid IP fragmentation if the data traffic
actually flows out to the physical Ethernet. Set a nonzero value here
*only* if you need to send the raw A/D stream over the Ethernet to
another host computer, and then only if you're *sure* that your
network can handle it. Be especially careful if you have a WiFi base
station on your network; even modest levels of sustained multicast
traffic will swamp a WiFi base station even if no client terminals are
subscribed to it.

**samprate** Default for *airspyd*: 20000000 (20 MHz). Default for
*airspyhfd*: 912000 (912 kHz). Set the A/D sample rate. Only certain
rates are supported by the hardware; they are listed to the syslog at
startup. The default is the maximum for each device.

**status-ttl** Default 1. Sets the Internet Protocol Time-to-Live (hop
count limit) field for status traffic. Unlike **data-ttl** the default
is 1 because the control program needs it and there is far less status
traffic than A/D data, so check this setting if it is getting status
responses from *radiod* but not *airspyd* or *airspyhfd* .  Note that
**status-ttl** = 1 will keep this traffic from passing through any
multicast routers, but the most likely use case for the *ka9q-radio*
package is on a single LAN. (Smart Ethernet switches are not routers;
they always pass multicast traffic to any port that subscribes to it.)

**ssrc** Default: derived from the time of day. Sets the 32-bit Real
Time Protocol (RTP) Stream Source identifier in the data stream. This
normally does not have to be set, as *radiod* and other consumers
automatically accept whatever SSRC is used.

**tos** Default: 48 (decimal). Sets the IP Type of Service header
byte, interpreted by many smart Ethernet switches (and routers) as a
priority or queue selector. The default value of 48 corresponds to
DSCP (Differentiated Services Code Point) AF12, which is one level
above "routine" traffic. This should not need to be changed as you
should engineer your LAN to have sufficient capacity to carry all this
traffic, and then priorities don't matter. However this field might
come in handy in configuring your switches to avoid being jammed by
multicast traffic should it flow by mistake somewhere it shouldn't
(e.g., a 10 Mb/s switch port).

Airspyd-only options
--------------------

**agc-high-threshold** Default -10 dBFS. Set the average A/D output
level at which the the software AGC will decrease the front end analog
gain by one step.

**agc-low-threshold** Default -40 dBFS. Set the average
A/D output level at which the software AGC will increase the front
end analog gain by one step. 

**bias** Boolean, default off. Enable the bias tee (preamplifier
power).

**lna-agc** Boolean, default off. Enable the hardware LNA AGC and
disable the software AGC. Doesn't seem to keep proper gain
distribution, i.e., poor sensitivity and/or excessive intermodulation
products seem to result. Use the default (software AGC) instead.

**mixer-agc** Boolean, default off. Enable the hardware mixer AGC and
disable the software AGC. Doesn't seem to keep proper gain
distribution, i.e., poor sensitivity and/or excessive intermodulation
products seem to result. Use the default (software AGC) instead.

**lna-gain, mixer-gain, vga-gain** Integers, defaults unset. Manually
set gains for the LNA, mixer and variable gain (baseband) analog
amplifier stages. The units are supposed to be in decibels but don't
seem well calibrated. Setting any of these values disables the
software AGC.

**gainstep** Integer, 0-21 inclusive, default unset. Manually select
an entry in the airspy library gain table and disable software AGC.
The default is to select an entry automatically with a software AGC
(in *airspyd*) based on the average A/D output level and the
*linearity* setting.

Airspyhfd-only options
----------------------

**hf-agc**  Default off. Exact function unknown.

**agc-thresh** Default off. Exact function unknown. Do not
confuse with the *airspyd* options **agc-high-threshold** and
**agc-low-threshold**.

**hf-att** Default off. Exact function unknown.

**hf-lna** Default off. Exact function unknown.

**lib-dsp**  Default on. Exact function unknown.
