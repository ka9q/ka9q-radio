*ka9q-radio* is a software defined radio for Linux I've been working on
for a few years. It is very different from most other amateur SDRs in
several respects:

1. Efficient multichannel reception. A single Raspberry Pi 4 can
simultaneously demodulate, in real time, every NBFM channel on a
VHF/UHF band (i.e., several hundred) with plenty of real time left
over.

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
or decode and relay digital messages (e.g., APRS, WSPR, Horus 4FSK,
radiosondes). These programs are automatically launched by the (new)
Linux standard system manager program *systemd*.

The core component is the *radiod* daemon. It accepts a (multicast) raw
A/D stream from a front end module and executes a configured set of
digital downconverters and simple demodulators for various linear and
FM modes, including AM, SSB, CW and a raw IQ mode intended mainly for
use by other programs.

Separate programs talk directly to several makes of SDR front end
hardware and generates the I/Q stream for *radiod*. These programs
currently include *airspyd* (Airspy R2), *airspyhfd* (Airspy HF+),
*rtlsdrd* (generic RTL-SDR dongles), *funcubed* (AMSAT UK Funcube Pro+),
*hackrf* (Great Scott Gadgets Hack RF One, receive only) and *rx888d* (RX-888 Mk II).

Two very rudimentary programs are provided for interactive use;
*monitor* listens to one or more demodulated audio streams and
*control* controls and displays the status of a selected receiver
channel.  It can also create and delete dynamic channel
instances. The *control* program uses a flexible and extensible
metadata protocol that could be (and I hope will be) implemented
by much more sophisticated user interfaces. Various utilities are
provided to record or play back signal streams, compress PCM audio
into Opus, pipe a stream into digital demodulators, etc.

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

Phil Karn, KA9Q
karn@ka9q.net

