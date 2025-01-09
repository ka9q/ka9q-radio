Configuring Receiver Channels in *ka9q-radio*  
Version 2.0 Jan 8, 2025, Phil Karn
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
least common multiple of the FFT bin spacing and the frame rate. For a 20 ms
frame, the frame rate is 50 Hz. For an overlap of 5 (i.e., 1/5 of each
FFT block is old data and 4/5 is new), the FFT bin spacing is 40 Hz. Therefore
the sample rate must be a multiple of 200 Hz.
This covers all the usual standard sample rates except for 44.1 kHz, the
sample rate of the compact disk.

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

### gain = 50 

Linear demodulator only (the FM and
WFM demodulators automatically set their output gains.)
This is the gain in dB applied to the output of the
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

### extend|threshold-extend = yes|no

FM only. Enable or disable an experimental FM threshold extension scheme to reduce the "popcorn" noise that happens just below what
is conventionally called "full quieting". This can buy another 2-3 dB until the pops become too frequent to suppress.

### tone|pl|ctcss = 0

FM only. When non-zero, enable a tone squelch for the specified tone frequency (Hz). Otherwise carrier squelch is used

### pacing = on|off

When on, add a delay after each transmitted Ethernet data packet to help avoid overrunning switches or hosts with
insufficient buffering. This is useful only at high bit rates where multiple packets need to be sent during each frame time.

The Ethernet data size limit is 1440, 60 bytes less than the standard Ethernet MTU (Maximum Transmission Unit) of 1500 bytes
to allow room for the RTP/UDP/IP headers. (The 1500 byte MTU excludes the 14-byte Ethernet header.) 1440 bytes every 20 ms (the usual
frame time) is 576 kb/s or 16-bit PCM at a 36 kHz sampling rate. Higher output rates (e.g., 48 kHz) require multiple packets be sent
with each frame's data, and without **pacing** on they are sent back-to-back. See also the **buffer** option.

### encoding = s16le|s16be|f16le|f32le|opus

Select an output encoding. All options except 'opus' are uncompressed
PCM (pulse code modulation). The default is s16be, i.e., signed 16-bit
linea PPM with big-endian byte order. This became an Internet
convention well before little-endian machines took over the world, so
programs like **pcmrecord** automatically convert this to s16le
(signed 16-bit little endian).

f32le is 32-bit IEEE 754 floating point format in little endian (i.e.,
Intel) byte order. This is ka9q-radio's internal format,
useful when a channel is operated with fixed gain to avoid exceeding
the (already large) 100 dB dynamic range of 16-bit linear PCM.

f16le is 16-bit IEEE 754 "half float" format. Half the size of f32le,
f16le still has a very large total dynamic range, though at some loss
in instantaneous dynamic range.

opus is the Opus codec defined in RFC 6716. Opus is an open, royalty
free "lossy" codec for general purpose use, from voice through high
fidelity audio. It is very widely used "under the hood" in web
browsing, games, and voice conferencing (e.g., Zoom, Signal.)  HF SSB
typically compresses to 15 kb/s and NBFM to 25 kb/s, an enormous
savings over uncompressed PCM rates (192 kb/s for 16 bit PCM @ 12 kHz
and 384 kb/s at 24 kHz.) Opus is highly recommended for signals that
you're just listening to, like FM or SSB voice, but stick to the PCM
formats for digital data signals that need to be decoded.

Note that Opus only supports a specific set of sample rates
(8/12/16/24/48 kHz) so ***nothing at all*** is sent unless the
demodulator sample rate is set to one of them. Opus also
supports only a specific set of frame durations (2.5, 5, 10, 20, 40,
60 ms, with more in newer versions) but this isn't a problem
at the default ka9q-radio frame rate of 20 ms.

### bitrate = 0

Set the target bitrate, in kb/s, for the Opus encoder. (No effect in
the PCM modes.) Setting this to 0 allows Opus to make its own
decisions according to the bandwidth and SNR of the audio signal. This
is usually the best choice.

### update = 25

Sets the interval, in frames, between transmissions of the status update packets
on the data channel. At a default frame time of 20 ms, this corresponds to 2 Hz, which
is good enough for most applications. Note that status updates stop in FM
when the squelch is closed.

These updates carry the full state of the channel and are needed by
programs such as **pcmrecord** and **monitor** to determine things
like the sample rate and format of the data stream before it can be
interpreted.

### buffer = 0

Controls data output buffering. Allowable values are 0-4.

The default of 0 means no buffering, i.e., at least one Ethernet
output packet is sent during every ka9q-radio frame. When delay is not
critical, buffering can significantly reduce the packet output rate at
lower sample rates or with the Opus codec.

Values 1-4 specify the maximum number of frame times that data may be
buffered pending additional data. In Opus, 0 and 1 are basically the
same: transmit one Opus packet every frame time. This is because the
Opus codec only handles a specific set of sample rates and packet
durations, and these match to the usual sample rates and frame times
of ka9q-radio (this is not a coincidence). Opus is so effective that
it is essentially impossible to reach the Ethernet size limit with 4 x
20 = 80 ms of audio, so this setting primarily sets the Opus frame
duration and encoding delay.

PCM modes are a little more complicated. They quickly run into the
Ethernet packet size limit; this happens with 16-bit samples at only
36 kHz (see the **pacing** section).  Nevertheless, Ethernet packets
can be much less than full at lower sample rates (e.g., 480 bytes for
16-bit PCM @ 12 kHz). Setting buffer = 3 will send 3 * 480 = 1440
bytes every 60 ms, cutting the packet rate to 1/3.  I recommend trying
this out especially in non-delay-sensitive applications like
WSPR/FT8/HFDL decoding, or when recording to files.

Data will never be delayed when there's enough to send a full packet,
so the effect of buffering is minimal at higher PCM sample
rates. Depending on the specific sample rate and format it still may
help fully pack all Ethernet frames instead of sending sequences of
full packets plus one partial fragment during each frame, still
reducing the average packet rate.

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
actual radio frequency (and other channel parameters) are included in
the metadata (status) stream, and newer versions of **pcmrecord** and
**monitor** now use this.


The Dynamic Template
--------------------

A dynamic "template" is also set up to allow new channels to be
created and deleted at run time, e.g., with the *control* command.
This is automatic if default **data** and **mode** settings are
present in the [global] section.



