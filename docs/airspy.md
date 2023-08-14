Configuring *ka9q-radio* to use the Airspy SDRs
===============================================
v2.0, August 2023
Phil Karn, KA9Q
---------------

Here is an excerpt of the relevant sections from one of my own *radiod* config files for the Airspy R2.
It is in 'ini' format.

[global]  
hardware = airspy  
status = 2m.local

[airspy]  
device = airspy  
description = "2m vertical"

The sections defining groups of receiver channels are omitted. See **ka9q-radio.md** for details on the options
for those sections.

Multiple instances of *radiod* can run on the same system, provided each has its own front end (they cannot be shared).
You can have as many as you want, subject to your CPU and USB limits.
(The Airspy R2 generates 240 Mb/s, but
it's a USB 2.0 -- not 3.0 -- device so each will have to be on its
own USB host controller.)

In the excerpt above, the **hardware** entry in the [global] section specifies the section containing Airspy configuration
information. (In this example the name of the hardware section happens to be the same as the device type, but it is not essential.)

Only one entry is mandatory: **device**. This specifies the front end hardware type, i.e, "airspy" (which means an Airspy R2) or "airspyhf" (the Airspy HF+).
The defaults should be good for most cases, but you can override them as needed.

**description** Optional. Gives free-format text that
will be advertised through the *radiod* program to the
control/status stream and the *control* program that
listens to it. It will also be advertised in DNS SRV (service
discovery) records by the Linux mDNS daemon *avahi*, so keep
it short but descriptive.

**serial** Optional. If not specified, *radiod* uses the first Airspy R2 or Airspy HF+ device discovered. Since this is probably
not what you want, you should explicitly specify the serial number if more than one is present.
This must exactly match the Airspy 64-bit serial
number, in hex (the leading 0x is optional).  This can be read from
the Airspy R2 with the *airspy_info* utility in the *airspy* Debian
package:

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

Reading the serial number from the Airspy HF+ requires the
*airspyhf_info* utility in the *airspyhf* Debian package:

>$ `airspyhf_info`  
AirSpy HF library version: 1.6.8  
S/N: 0x3652D65D4ACB39F8  
Part ID: 0x00000002  
Firmware Version: R3.0.7-CD  
Available sample rates: 912 kS/s 768 kS/s 456 kS/s 384 kS/s 256 kS/s 192 kS/s  
   
Note that *airspy_info* (or *airspyhf_info*) will not see the device when any other
program (including *radiod*) has it open.
If the serial number is specified for a non-existent device, *radiod* will exit and Linux
*systemd* will restart it every 5 seconds until the device appears.

I find it very helpful to externally label each of my Airspy devices with
their serial numbers.

**samprate** Integer, default is the highest speed advertised by the
device, usually 20 MHz for the Airspy R2 and 912 kHz for the
Airspy HF+. 
This sets the A/D sample rate. Note that the Airspy R2 is
typically described as producing complex samples at 10 MHz. However,
there's actually only one A/D converter that can sample at 20 MHz; the
real->complex conversion and half-rate decimation is performed in the airspy library. Since *radiod* performs
a FFT on its input stream that can accept either real or
complex samples, it is considerably faster to bypass the library
conversion and accept the raw real-valued samples.
On the other hand, the current Airspy HF+ library readily supports only complex output samples.
The supported sample rates are logged in */var/log/syslog* when *radiod* starts and the device is initialized

Airspy R2-only options
--------------------

**linearity** Boolean, default off. Like most second-generation SDRs
with analog tuners, the Airspy R2 has three stages of analog gain
ahead of the A/D converters that any AGC must carefully manage. The
Airspy library provides separate gain tables optimized for sensitivity
and for linearity (i.e. resistance to intermod). The
sensitivity table is used by default, but in areas with strong signals the
linearity table may provide better resistance to intermod. I'm about 6
km line of sight from a dozen FM broadcast transmitters so I often use the
linearity setting.

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
based on the average A/D output level and the
*linearity* setting.

Airspy HF+ -only options
----------------------

**hf-agc**  Default off. Exact function unknown.

**agc-thresh** Default off. Exact function unknown. Do not
confuse with the *airspy* options **agc-high-threshold** and
**agc-low-threshold**.

**hf-att** Default off. Exact function unknown.

**hf-lna** Default off. Exact function unknown.

**lib-dsp**  Default on. Exact function unknown.
