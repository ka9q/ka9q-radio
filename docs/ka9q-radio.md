Configuring *ka9q-radio*
========================

v0.1 (in progress), May 2022  
Phil Karn, KA9Q
---------------

A Linux system can run any number of instances of *radiod* (the
ka9q-radio radio daemon) subject to resource limits (CPU and Ethernet
capacity).

Each *radiod* instance has a configuration file, e.g.,
*/etc/radio/radiod@2m.conf*, where "2m" is the instance name.  Each
*radiod* instance reads a raw A/D sample stream from a front end
module, e.g., *airspyd*. More than one *radiod* instance can read from
the same front end, but it is most efficient to have just one instance
per front end handling all the channels.

*radiod* has (too?) many settable parameters but most have reasonable
defaults and should not need to be changed. Only a few must be
configured for your situation.

Here is an excerpt from *radiod@2m.conf* on my system:

>[global]  
>input = 2m-vertical.local  
>status = 2m.local  
  
>[2m FM]  
>mode = pm  
>data = 2m-pcm.local  
>freq = "145m800 144m490 145m200 145m825 145m990 144m310 144m325 144m340 144m355"  

There must be exactly one [global] section in a config file, and it
applies to the entire *radiod* instance. This [2m FM] section
configures nine frequency channels sharing the same mode and output
multicast group.  (The nine channels are still distinguished by RTP
SSRCs so they can be individually distinguished by multicast
listeners.)

Not every parameter is meaningful in every context; some are limited
to the [global] section because they necessarily apply to the entire
*radiod* instance.  The special parameter **mode** invokes a set of
parameters from */usr/local/share/ka9q-radio/modes.conf* (which might
be better called a "preset table" for this reason). Parameters set in
*modes.conf* may in turn be individually overridden by settings in the
[global] section and/or a channel group, with the latter taking
precedence.

New "modes" can be easily added to *modes.conf*; this is recommended
if you find yourself changing them often.

Parameters
----------

The available parameters are as follows. Unless
otherwise specified, each may be given in the [global] section, a
channel group section, or in *modes.conf*.

**blocktime** Decimal; default 20 milliseconds (i.e., 50 Hz block
rate). Valid only in the [global] section.  Specifies the duration of
the forward FFT shared by all receiver channels.  The actual block
size in samples is equal to **blocktime** times the A/D sample
rate. E.g., the Airspy R2 has a 20 MHz (real) sample rate, so a 20 ms
**blocktime** corresponds to a block of 400,000 real samples. The actual
FFT block is larger because it includes data from the previous block
depending on the **overlap** setting.

Larger values of **blocktime** permit sharper channel filters and are
more tolerant to CPU scheduling latencies but add latency and incur
greater CPU overhead. (The relative per-sample CPU cost of an FFT
increases with the square root of the FFT block size.)

**overlap** Integer; default 5. Valid only in the [global]
section. Sets the ratio of old to new samples in each forward FFT. An
overlap of 5 means that 1/5 of each FFT input block consists of A/D
samples from the previous block and 4/5 are new A/D samples. This sets
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

**input** String. Required, no default. Valid only in the [global]
section. Specifies the DNS name of the
control/status group of the SDR front end module, e.g., the **status**
entry in */etc/radio/airspyd.conf*. The name is resolved to an IP
address with multicast DNS through the Linux *avahi* daemon, so
*radiod* will block indefinitely if the front end module isn't
running. Note that it is not necessary to specify the multicast group
for the front end data stream; *radiod* gets this information from
the status stream.

**samprate** Integer; default 24000 (24 kHz). Specifies the default
PCM output sample rate for each receiver channel.

**data** String; no default. Not valid in *modes.conf*.
Specifies the DNS name of the multicast
group to be used for receiver output streams. If not specified in the
[global] section, it must be individually specified in each subsequent
section. *radiod* will deterministically hash this string to generate and advertise
an IPv4 multicast address in the 239.0.0.0/8 block, along
with a SRV DNS record of type _rtp._udp advertising this name.

A single output stream can carry many receiver channels, each
distinguished by its 32-bit RTP SSRC (Real Time Protocol Stream Source
Identifier), which must be unique for an instance of
*radiod*. However, consider that Ethernet switches, routers and host
handle multicast group subscriptions by their IP addresses only, so an
application (e.g., *monitor*) will receive and discard traffic from
any unwanted SSRCs sharing an IP multicast address with the desired
traffic. At a 24 kHz sample rate, each 16-bit mono PCM stream is 384
kb/s plus header overhead, so this can add up when many channels are
active.  This is usually OK on 1Gb/s Ethernet, but it can be a problem
over slower Ethernets or WiFi, especially where the base station does
not do multicast-to-unicast conversion. In these cases, use the Opus
transcoder daemon *opusd*.

**mode** String; no default. Not valid in *modes.conf*. Required in
either [global] or in each receiver group section.  Loads a related
set of parameters from the specified section of
*/var/lib/ka9q-radio/modes.conf*. See the documentation for that file
for a description of the standard definitions.

**status** String; No default. Valid only in [global]. Specifies the
domain name of the metadata multicast group.  In a change from earlier
versions there is now only one status group per instance of *radiod*,
and status information is multicast only in response to a command
(which may be empty). Not mandatory, but unset there will be no way to
dynamically create new receiver channels or to control or monitor
statically configured channels. *radiod* will deterministically hash
this string to generate and advertise an IPv4 multicast address in the
239.0.0.0/8 block, along with a SRV DNS record of type _ka9q-ctl._udp
with this name.

**tos** Integer; default 48. Not valid in *modes.conf*.  Sets the
IP Type of Service (TOS) field used in all outgoing packets. See the
discussion in airspy.md for further details.

**ttl** Integer; default 1. Not valid in *modes.conf*. Sets the IP
Time-to_Live field for all outgoing packets.  See the discussion in
airspy.md.

**fft-threads** Integer; default 1. Valid only in [global]. Sets the
number of threads to be used by FFTW3 for the forward FFT shared by
all the receiver channels.  I added this when I thought multithreading
was necessary to make *radiod* run in real time on the Raspberry Pi;
having found other ways to improve performance I recommend the default
except for experimenting. Multithreading FFTW3 may decrease the
latency of each FFT, but at the cost of greater total CPU time across
all cores.

**rtcp** Boolean; default off. Valid only in [global]. Enable the Real
Time Protcol (RTP) Control protocol. Incomplete and experimental;
leave off for now.

**sap** Boolean; default off. Valid only in [global]. Enable the
Session Announcement Protocol (SAP). Eventually this will make
receiver streams visible to session browers in applications such as
VLC. Leave off for now.

**mode-file** String; default */usr/local/share/ka9q-radio/modes.conf*. Valid only in [global].
Specifies the mode
description file mentioned in the **mode** parameter above. Use the
default.

**wisdom-file** String; default */var/lib/ka9q-radio/wisdom*. Valid only in [global].
Specifies
where FFTW3 should store accumulated "wisdom" information about the
fastest ways to perform *radiod*'s specific FFT transforms on this
specific CPU.  FFTW3 also uses the "global wisdom" file
*/etc/fftw/wisdomf*. Right now I generate the latter file by hand with a fairly esoteric
set of commands (see FFTW3.md). FFTW *can* generate this information
automatically when first run but can take *hours* to
do so. I am working on a better way, e.g., by
automatically starting wisdom generation in the background so that
*radiod* starts immediately, though of course it will run faster after wisdom
generation is complete and *radiod* is restarted to use it.

**demod** 3-valued string: "Linear", "FM" and "WFM". Selects one of
three demodulators in *radiod*. The "Linear" demodulator is for modes
such as AM (envelope detected or coherent), SSB, CW, IQ and
DSB-SC. The "FM" demodulator is for general purpose frequency
modulation, including so-called "NBFM" that is actually phase
modulation (PM). "WFM" is similar to FM but is intended for FM stereo
broadcasting; it contains a multiplex decoder and always produces
stereo output at a 48 kHz sample rate. (Note: a separate *stereod*
daemon may be used to decode multiplexed stereo from the single
channel composite baseband output of a regular FM demodulator with
appropriate bandwidth and sample rates, but the WFM demodulator is
more convenient.)

**samprate** Decimal, default 24000 (24 kHz). Set the output sample rate.

**channels** Integer, default 1. Number of output channels, must be 1
or 2. Forced to 1 for FM modes, 2 for WFM (Wideband broadcast FM). In
linear mode with envelope detection disabled, 2 implies I/Q
output. With envelope detection enabled, 2 channels places the linear
output on the left channel and the evelope detector output on the
right channel.  This is for experimentation with automatic SSB tuning
algorithms that use envelope detection to determine voice pitch
frequencies.

**stereo** Boolean. Equivalent to **channels** = 2.

**mono** Boolean. Equivalent to **channels** = 1.

**disable** Boolean, default off. Channel group entry only. Disables a channel group.

**kaiser-beta** Decimal, default 11.0. Sets the Kaiser window beta
parameter for filter generation. Larger values give better sidelobe
suppression while smaller values narrow the main lobe (i.e., give
faster transitions between passband and stopband). Don't be tempted to
lower this parameter too much, as windowing also prevents aliasing
during sample rate downconversion. Once aliases get into a digital
signal, there's no getting rid of them. If you need sharper filters,
use a larger **blocksize** or a smaller **overlap** factor (e.g., 2).

**low** Decimal, default -5000 (-5 kHz). Sets the lower edge of the predetection filter passband.

**high** Decimal, default +5000 (+5 kHz). Sets the upper edge of the predetection filter passband.

Both **low** and **high** are set by every entry in *modes.conf*, so
these defaults just provide a backstop in case an entry lacks them.

**squelch-open** Decimal, default 8 dB. Sets the SNR at which the squelch opens.

**squelch-close** Decimal, default 7 dB. Sets the SNR at which the
squelch closes. The difference provides some hysteresis.

Squelch is applicable to both FM and AM, though very different
algorithms are used. In FM, the SNR is estimated from the mean and
variance of the signal amplitude; since FM is constant envelope, any
variance is due to noise.  The AM squelch uses a PLL to detect a
carrier, so it is active whenever the PLL is turned on. Note that it
is still active even if envelope detection is selected. If synchronous
AM detection is wanted without squelch, set the opening threshold to
-1000 dB.  This will underflow to -infinity dB and keep the squelch
open.

**squelchtail** Integer, default 1 block. Keep the squelch open for
the specified number of block times (e.g., 20 milliseconds each) after
the SNR drops below the **squelch-close** threshold. The default of 1
block keeps the ends of packet transmissions from being chopped off,
while a value of 0 is suitable to completely eliminate audible squelch
tails in FM voice operation.

**headroom** Decimal, default -15 dBFS. Sets the target output audio
level. Valid in all modes but relevant mainly to the linear
demodulator (e.g., SSB/CW/AM) with AGC enabled. All signal level
measurements are averages over a block time (default 20 ms), so this
setting minimizes clipping on signals with the usual Gaussian (noise like) statistics.

**shift** Decimal, default 0 Hz. Linear demodulator only, applicable
mainly to CW operation.  Sets the frequency shift to be applied after
downconversion, baseband filtering and PLL tracking, if enabled. Not
the same as offsetting the predetection filter and adjusting the
tuning frequency to compensate because those operations are performed
before the 0-Hz PLL while the **shift** value is applied last.

**recovery-rate** Decimal, default +20 dB/s. Linear demodulator
only. Specifies the rate at which gain is to be recovered when the
input signal level has decreased and the **hang-time** has expired.

**hang-time** Decimal, default 1.1 s. Linear demodulator only. Specifies the time to hold
a constant gain after the input signal level decreases.

**threshold** Decimal, default -15 dB. Linear demodulator
only. Specifies the output level, relative to the **headroom**
setting, that the AGC will maintain on noise without signal. With the
default **headroom** setting of -15 dBFS, noise will thus appear at
-30 dBFS.

**gain** Decimal, default 0 dB. Linear demodulator only (the FM and
WFM demodulators force this to fixed values depending on the
predetection bandwidth.) This is the gain applied to the output of the
linear demodulator before transmission to the output data stream. With
the AGC on, this value only sets the initial value before the AGC
operates. It should be chosen to avoid either loud momentary bursts (from being set too high) or
excessive gain recovery time (from being set too low).

**envelope** Boolean, default off. Select the AM envelope detector
(linear demodulator only). When 2 channels are selected, send the
envelope detector to the right channel (see the discussion under
**channels**).

**pll** Boolean, default off. Linear demodulator only. Enable the
phase lock loop. When enabled, the PLL will try to track a carrier to
a nominal frequency of 0 Hz.

**square** Boolean, default off. Linear demodulator only. Enables the
PLL with a squaring operation in the feedback path. Useful for DSB-SC
(Double Sideband AM with Suppressed Carrier). Implies **pll** on.

**conj** Boolean, default off. *Currently unimplemented*. Linear demodulator only.
Enables independent sideband operation: LSB on left channel, USB on right channel.

**pll-bw** Decimal, default 100 Hz. Linear demodulator only. Sets the
loop filter bandwidth of the PLL.

**agc** Boolean, default on. Linear demodulator only. Enables automatic gain control (AGC).

**deemph-tc** Decimal. Not applicable to the linear
demodulator. Default: 530.5 (FM demodulaor), 75 (WFM
demodulator). Sets the time constant, in microseconds, for FM
demodulation, that controls the transition between a "flat FM" response for lower frequencies to
phase modulation at higher frequencies (a de-emphasis rate of -6 dB/octave).

The 530.5 microsecond value corresponds to a corner frequency of 300
Hz, the apparent but unofficial value for amateur narrowband FM, while
75 microseconds is the official value for FM broadcasting in North
America and South Korea. Other countries use 50 microseconds.

**deemph-gain** Decimal. Not applicable to the linear
demodulator. Default: +12 dB (empirically chosen) for the FM
demodulator, 0 db for the WFM demodulator. Applies the specified
amount of gain to the signal after de-emphasis to maintain the same
objective loudness as the demodulator output with de-emphasis
off. Needed mainly for NBFM because its 300 Hz corner frequency is
below the nominal speech band. Not needed for WFM because the 75
microsecond time constant corresponds to 2123 Hz, above most of the
power in typical speech or music.

**freq** Decimal, no default. Set the channel carrier frequency.  If a
list is specified, a receiver channel will be created for each
one. Automatically sets the RTP SSRC if not otherwise specified to the
number formed by removing all letters from the frequency string.

Frequencies are specified either as pure decimal numbers, or with a SI scale factor as follows:

123.4k, 123k4 - 123.4 kilohertz  
123.4m, 123m4 - 123m4 Megahertz  
1.234g, 1g234 - 1.234 Gigahertz  

When no scale is given, a heuristic is used to give a "reasonable"
answer (which may not always be right). e.g., "400" is interpreted as
400 MHz while 700 is interpreted as 700 kHz. When in doubt, specify
the scale factor or give the frequency in hertz.

A frequency of 0 has special meaning: it creates a "prototype"
receiver channel that will be cloned when the *control* program
specifies a previously unknown SSRC.

Ten aliases for **freq** are provided, **freq0** through **freq9**. This is strictly to work
around the line length limitation in the *libiniparser* library so you can have literally
hundreds of receiver channels in a single *radiod* instance. (The parser only allows one of each key to be present in a section, so repeating **freq** won't work.)

**ssrc** Decimal, default based on frequency as described above. Set
the RTP (Real Time Protocol) SSRC (Stream Source Identifier). This is
a 32-bit value placed in every RTP header to identify the stream,
necessary because many streams may share the same IP multicast group
from the same originating host.

Note that once the SSRC is set for a stream it cannot be changed even
if the channel is tuned to a different radio frequency, making this
usage a bit of a hack.  But particularly in VHF/UHF repeater
monitoring, most *ka9q-radio* channels are statically configured and
never retuned, so it is nonetheless a useful convention. Also, the
SSRC can still be used to find the actual radio frequency (and
other channel parameters) from the metadata (status) stream.





