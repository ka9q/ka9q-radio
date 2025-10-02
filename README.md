*ka9q-radio* is a software defined radio for Linux I've been working on
for a few years. It is very different from most other amateur SDRs in
several respects:

1. Efficient multichannel reception. A single Raspberry Pi 4 can
simultaneously demodulate, in real time, every NBFM channel on a
VHF/UHF band (i.e., several hundred) with plenty of real time left
over.

A mid-range x86 can handle the RX888 MkII at full sample rate
(129.6 MHz), receiving multiple channels sumultaneously on all
LF/MF/HF ham bands (or anything else) plus 6 meters.

A Raspberry Pi 5 can handle the RX888 MkII at half sample rate (64.8
MHz), enough to receive up to 30 MHz. (An external hardware anti-alias
filter is required, and "FFTW Wisdom" must be generated for the Pi5 to
run this fast.)

2. All I/O (both signal and control/status) uses IP multicasting.
This makes it easy for more than one module, on the same computer or
on a LAN, to operate on the outputs of other modules, or for
individual modules to be restarted without restarting everything else.

If you want a user-friendly, interactive, graphics-laden SDR with a
simple learning curve, then *ka9q-radio* is NOT what you're looking
for! (At least not yet.) Try one of the many excellent SDR programs
already available like SDR#, Cubic SDR, gqrx, etc, or the standalone
Kiwi SDR.  This is my personal experiment in building a very different
kind of SDR that runs as a component serving other applications.

The core components in *ka9q-radio* run as Linux 'daemons' (background
programs) with little (or no) user interaction. Turnkey systems can be
configured to, e.g., demodulate and record every FM channel on a band,
or decode, log and/or relay digital messages (e.g., APRS, FT-8, WSPR, HFDL, Horus 4FSK,
radiosondes). These programs are automatically launched by the (new)
Linux standard system manager program *systemd*.

The core component is the *radiod* daemon. It reads an A/D stream
directly from a front end and executes a configured set of digital
downconverters and simple demodulators for various linear and FM
modes, including AM, SSB, CW and a raw IQ mode intended mainly for use
by other programs.

Until mid-2023, ka9q-radio had separate programs (e.g., *airspyd*) for
talking to several makes of SDR front end hardware and generated an
I/Q multicast stream for *radiod*. Because of performance problems,
code and configuration complexity and general lack of utility these
separate programs have been obsoleted and the front end drivers built
directly into *radiod*. Support is currently provided for generic
RTL-SDR dongles, the Airspy R2, Airspy HF+, AMSAT UK Funcube Pro+ and
RX-888 Mk II. The Rigexpert Fobos, SDRplay, BladeRF, HackRF, and
HydraSDR are now optionally supported.

A synthetic front end, *sig_gen*, is
also provided. It simulates a front end, either complex or real,
producing gaussian noise and single carrier at specified amplitudes.
It can also transmit my WWV/H simulator *wwvsim*, but it's not yet
well integrated, mainly because of the need for an external speech synthesizer.

Support will be forthcoming for the HackRF (receive
only).

Two very rudimentary programs are provided for interactive use;
*monitor* listens to one or more demodulated audio streams and
*control* controls and displays the status of a selected receiver
channel.  It can also create and delete dynamic channel
instances. The *control* program uses a flexible and extensible
metadata protocol that could be (and I hope will be) implemented
by much more sophisticated user interfaces. Various utilities are
provided to record or play back signal streams, compress PCM audio
into Opus, pipe a stream into digital demodulators, etc.

*Radiod* periodically multicasts ("beacons") status information on
each output stream and user programs are being enhanced to make
use of it. For example, *monitor* now displays the frequency, mode and
signal-to-noise ratio of each channel. The *pcmrecord* program automatically
determines sample rate, format and byte order; with the options --stdout
and --exec, it obsoletes the older programs *pcmcat* and *pcmspawn*, respectively.

Although I've been running all this myself for several years, it is
NOT yet ready for general use. A LOT of work still remains, especially
documentation. But you're welcome to look at it, make comments and
even try it out if you're feeling brave. I would especially like to
hear from those interested in building it into their own SDR
applications.

My big inspiration for the multichannel part of my project was this
most excellent paper by Mark Borgerding: "Turning Overlap-Save into a
Multiband Mixing, Downsampling Filter Bank". You probably won't
understand how it works until you've read it:

https://www.iro.umontreal.ca/~mignotte/IFT3205/Documents/TipsAndTricks/MultibandFilterbank.pdf

Although there are other ways to build efficient multichannel
receivers, most notably the polyphase filter bank, fast convolution is
extraordinarily flexible. Each channel is independently tunable with
its own sample rate and filter response curve. The only
requirement is that the impulse response of the channel
filters be shorter than the (configurable) overlap interval in the forward
FFT.

Updated 2 Feb 2025  
Phil Karn, KA9Q  
karn@ka9q.net


