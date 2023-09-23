Configuring Receiver Channels in *ka9q-radio*  
Version 1.0 April 2023, Phil Karn
=============================================

(This is the third part of the *ka9q-radio* configuration guide.
The [global] and hardware sections are described in [ka9q-radio.md](ka9q-radio.md)
and [ka9q-radio-2.md](ka9q-radio.md) respectively.)

A *radiod* configuration file may contain one or more sections
configuring receiver channels. This
is necessarily the most complex part of the configuration since
it depends on your specific applications.

Each section configures one or more channels with the same mode,
output multicast stream and sample rate, differing only in frequency. What follows
is a list of parameter settings with their default values:

### mode = pm|npm|fm|nfm|wfm|am|cam|ame|iq|cwu|cwl|usb|lsb|dsb|amsq|wspr|spectrum

No default. Required unless specified in [global].

A "mode" would be more accurately called a "preset" because it
actually refers to a group of parameters (demodulator type, sample
rate, filters, etc) defined in sections of
*/var/lib/ka9q-radio/modes.conf*. See the documentation for that file
for a description of the standard definitions.

The *modes.conf* file covers the most common modes, but individual parameters can be
overridden if desired. If you find yourself doing this a lot, you might
add a new mode to */var/lib/ka9q-radio/modes.conf*. Be careful to back
it up; it may be overwritten by the next "make install.

### data = 

No default. Required unless specified in [global].

Specifies the DNS name of the multicast group to be used for receiver
output streams. If not specified in the [global] section, it must be
individually specified in each subsequent section.

*radiod* deterministically hashes the destination string to generate
and advertise an IPv4 multicast address in the 239.0.0.0/8 block,
along with a SRV DNS record of type _rtp._udp advertising this name.
See the discussion of this parameter in the [global] section.


Parameters in *modes.conf*
--------------------------

The parameters that may be set in *modes.conf* and selectively overridden
in each receiver channel group are:

### demod = linear|fm|wfm

Selects one of
three demodulators built into *radiod*, distinct from the
**mode** entries that select an entry in
*/usr/local/share/ka9q-radio/modes.conf*.  The "Linear" demodulator is
for modes such as AM (envelope detected or coherent), SSB, CW, IQ and
DSB-SC. The "FM" demodulator is for general purpose frequency
modulation, including so-called "NBFM" that is actually phase
modulation (PM). "WFM" is similar to FM but is intended for FM stereo
broadcasting; it contains a multiplex decoder and always produces
stereo output at a 48 kHz sample rate. (Note: a separate *stereod*
daemon may be used to decode multiplexed stereo from the single
channel composite baseband output of a regular FM demodulator with
appropriate bandwidth and sample rates, but the WFM demodulator is
more convenient.)

### samprate =

Set the output sample rate in Hz. A good value for communications
FM/PM is 24000 (24 KHz); for linear modes, 12000 (12 kHz). WFM (FM
stereo broadcast) should use 48000 (48 kHz). (These values are used on
**modes.conf**.)

Remember the Nyquist theorem: the sample rate must be at least twice
the highest frequency. For real signals, such as mono audio,
a sample rate of 12 kHz can handle audio up to 6 kHz, but you must allow
room for filter skirts so don't push the filters beyond about +/-5 kHz.
If you need more, increase the sample rate.

FM is a little different; the sample rate is constrained by the
pre-demodulation bandwidth of the signal, not the audio output. NBFM
is typically 16-20 kHz wide and is handled internally as a *complex*
sample stream, so a sample rate of 24 kHz is sufficient.  (Since the
demodulated audio output has at most a bandwidth of 5 kHz, it doesn't
need such a high sample rate. A future enhancement of *ka9q-radio* may
lower the sample rate of the FM demodulator output to save network
capacity.)

*ka9q-radio* supports any output sample rate that is a multiple of the
FFT bin spacing.  This is equal to (overlap - 1) / (overlap *
blocktime).  For the default of block time = 20 ms and overlap = 5,
that's 4/(5 * 20 ms) = 40 Hz. This covers all the usual standard sample rates.

### channels = 1|2

Set the number of output channels. Forced to 1 for FM modes, 2 for WFM
(Wideband broadcast FM). In linear mode with envelope detection
disabled, ***channels = 2*** implies I/Q output. With envelope detection enabled,
***channels = 2*** places the linear output on the left channel and the evelope
detector output on the right channel.  This is for experimentation
with automatic SSB tuning algorithms that use envelope detection to
determine voice pitch frequencies.

### stereo = y

Equivalent to **channels = 2**.

### mono = y

Equivalent to **channels = 1**.

### kaiser-beta = 11.0

Sets the [Kaiser window](https://en.wikipedia.org/wiki/Kaiser_window)
beta parameter for filter generation. Larger values give better sidelobe
suppression while smaller values narrow the main lobe (i.e., give
faster transitions between passband and stopband). Don't be tempted to
lower this parameter too much, as windowing also prevents aliasing
during sample rate downconversion. Once aliases get into a digital
signal, there's no getting rid of them. If you need sharper filters,
use a larger **blocksize** or a smaller **overlap** factor (e.g., 2) in the [global] section.

### low = -5000

Sets the lower edge of the predetection filter passband.

### high = +5000

Sets the upper edge of the predetection filter passband.

Both **low** and **high** are set by every entry in *modes.conf*, so
these defaults just provide a way to override them when necessary.

### squelch-open = 8

Sets the SNR (in dB) at which the squelch opens.

### squelch-close = 7

Sets the SNR (in dB) at which the
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

### squelchtail = 1

Keep the squelch open for
the specified number of block times (e.g., 20 milliseconds each) after
the SNR drops below the **squelch-close** threshold. The default of 1
block keeps the ends of packet transmissions from being chopped off,
while a value of 0 is suitable to completely eliminate audible squelch
tails in FM voice operation. Because the preset 'pm' in modes.conf
is usually used for ordinary voice, it sets this to 0. Preset 'fm' (no de-emphasis) sets it to 1.

### headroom = -15 

Sets the target output audio
level in dBFS. Valid in all modes but relevant mainly to the linear
demodulator (e.g., SSB/CW/AM) with AGC enabled. All signal level
measurements are averages over a block time (default 20 ms), so this
setting minimizes clipping on signals with the usual Gaussian (noise like) statistics.

### shift = 0

Sets the frequency shift applied after
downconversion, baseband filtering and PLL tracking, if enabled. Not
the same as offsetting the predetection filter and adjusting the
tuning frequency to compensate because those operations are performed
before the 0-Hz PLL while the **shift** value is applied last. Applicable
to the linear demodulator only. Primarily used for CW.

### recovery-rate = +20 

Specifies the rate, in decibels per second, at which gain is increased
when the input signal level has decreased and the **hang-time** has
expired.  Linear demodulator only.

### hang-time = 1.1

Specifies the time to hold
a constant gain after the input signal level decreases.
Linear demodulator only.

### threshold = -15

Specifies the output level in dB relative to the **headroom**
setting, that the AGC will maintain on noise without signal. With the
default **headroom** setting of -15 dBFS, noise will thus appear at
-30 dBFS. Applicable only to the Linear demodulator.

## gain = 50 

Linear demodulator only (the FM and
WFM demodulators force this to fixed values depending on the
predetection bandwidth.) This is the gain in dB applied to the output of the
linear demodulator before transmission to the output data stream. With
the AGC on, this value only sets the initial value before the AGC
operates. It should be chosen to avoid either loud momentary bursts (from being set too high) or
excessive gain recovery time (from being set too low).

### envelope = on|off

Select the AM envelope detector
(linear demodulator only). When 2 channels are selected, send the
envelope detector to the right channel (see the discussion under
**channels**). Linear only.

### pll = on|off

Linear demodulator only. Enable the
phase lock loop. When enabled, the PLL will try to track a carrier to
a nominal frequency of 0 Hz.

### square = on|off

Linear demodulator only. Enables the
PLL with a squaring operation in the feedback path. Useful for DSB-SC
(Double Sideband AM with Suppressed Carrier). Implies **pll = on**.

### conj = on|off

*Currently unimplemented*. Linear demodulator only.
Enables independent sideband operation: LSB on left channel, USB on right channel.
Linear only.

### pll-bw = 100

Linear demodulator only. Sets the
loop filter bandwidth of the PLL in Hz.

### agc = on|off

Linear demodulator only. Enables automatic gain control (AGC).

### deemph-tc = 530.5

Not applicable to the linear
demodulator. Default: 530.5 (FM demodulator), 75 (WFM
demodulator). Sets the time constant, in microseconds, for FM
demodulation. This controls the transition between a "flat FM"
response at lower frequencies to phase modulation (-6 dB/octave
de-emphasis) at higher frequencies.

The 530.5 microsecond value corresponds to a corner frequency of 300
Hz, the apparent unofficial value for amateur narrowband FM, while
75 microseconds is the official value for FM broadcasting in North
America and South Korea. Other countries use 50 microseconds.

### deemph-gain = +12

Not applicable to the linear
demodulator. Default: +12 dB (empirically chosen) for the FM
demodulator, 0 db for the WFM demodulator. Applies the specified
amount of gain to the signal after de-emphasis to maintain the same
objective loudness as with de-emphasis
off. Needed mainly for NBFM because its 300 Hz corner frequency is
below the nominal speech band. Not needed for WFM because the 75
microsecond time constant corresponds to 2123 Hz, above most of the
power in typical speech or music.


### tos = 48

Sets the IP Type of Service (TOS) field used in all outgoing packets, overriding
any value set in [globa].

### ttl = 0

Sets the IP Time-to_Live field for all outgoing packets, overriding
any value set in [global].


### disable = yes|no

A section may be disabled without deleting it by setting "disable = yes"
somewhere in the section.


### freq = 

No default. Set the channel carrier frequency.  If a
list is specified, a receiver channel will be created for each
one. Automatically sets the RTP SSRC if not otherwise specified to the
number formed by removing all letters from the frequency string.

Frequencies are specified either as pure decimal numbers, or with a SI scale factor as follows:

>123.4k, 123k4 - 123.4 kilohertz  
>123.4m, 123m4 - 123m4 Megahertz  
>1.234g, 1g234 - 1.234 Gigahertz  

When no scale is given, a heuristic is used to give a "reasonable"
answer (which may not always be right). e.g., "400" is interpreted as
400 MHz while 700 is interpreted as 700 kHz. When in doubt, specify
the scale factor or give the frequency in hertz.

Ten aliases for **freq** are provided, **freq0** through **freq9**. This is strictly to work
around the line length limitation in the *libiniparser* library so you can have literally
hundreds of receiver channels in a single *radiod* instance. (The parser only allows one of each key to be present in a section, so repeating **freq** won't work.)

### ssrc = 

Set the RTP (Real Time Protocol) SSRC (Stream Source Identifier). This
is a 32-bit value placed in every RTP header to identify the stream,
necessary because many streams may share the same IP multicast group
from the same originating host.

Normally the SSRC is set automatically from the frequency, as described above.

Note that once the SSRC is set for a stream it cannot be changed even
if the channel is tuned to a different radio frequency, making this
usage a bit of a hack.  But particularly in VHF/UHF repeater
monitoring, most *ka9q-radio* channels are statically configured and
never retuned, so it is nonetheless a useful convention. Also, the
SSRC can still be used to find the actual radio frequency (and other
channel parameters) from the metadata (status) stream.


The Dynamic Template
--------------------

A dynamic "template" is also set up to allow new channels to be
created and deleted at run time, e.g., with the *control* command.
This is automatic if default **data** and **mode** settings are
present in the [global] section.



