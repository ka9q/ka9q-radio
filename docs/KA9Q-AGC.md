Automatic Gain Control (AGC) in *ka9q-radio*  
v 1.1, Phil Karn KA9Q, August 2023
============================================


Front End AGC
-------------

The *ka9q-radio* package has two independent AGCs with very different
purposes. The first keeps the front end A/D converter within range,
i.e., not driving it into saturation while also not degrading the
overall noise figure too much, hopefully without affecting signal
levels seen by the second AGC at the end of the signal path. When the
A/D output exceeds an upper threshold, the analog gain ahead of the
A/D converter is reduced in steps; when it falls below a lower
threshold, it is increased. These changes are compensated for with an
equal and opposite digital gain change immediately after the
conversion to 32-bit floating point format used inside *ka9q-radio*.

The intent is to maintain a constant overall gain from the antenna
terminal. This keeps the second AGC (in *radiod*'s linear demodulator,
discussed below) from seeing abrupt level changes caused by the first
AGC.  This is only partly effective, mainly because the analog gains
are not well calibrated, but also because of the small time delay
between an analog gain command to the front end and the resulting
change in digital signal level.  But it works well in practice as long
as front end gain changes aren't too frequent (hysteresis minimizes
this). An antenna produces a sum of many transmitters and noise
sources with fairly stationary (i.e, stable) Gaussian statistics. It
only changes significantly when a strong nearby transmitter keys on or
off.  Such situations, especially with strong pulsed signals on
VHF/UHF, may require ad-hoc manual gain settings.

Second generation SDR front ends (i.e., those with analog tuners)
typically have three configurable analog gain stages: LNA, mixer and
IF (or baseband).  Some have a switchable bias tee to power an
external preamp.

Proper gain distribution is essential to optimize overall sensitivity
and linearity (i.e., susceptibility to intermodulation
distortion). The vendor-supplied library for the Airspy R2 comes with
separate gain tables optimized for either sensitivity or linearity,
but most other SDR front ends do not. Even these tables probably need
modification when used with an external preamp.


The Linear Demodulator AGC in *radiod*
--------------------------------------

This AGC is similar to those in traditional communications receivers
but with some enhancements I believe are novel. It operates after
downconversion and filtering so it responds only to in-channel signals;
each channel has its own AGC.

The AGC is controlled by signal energy measurements made on each block
of baseband samples.  The block length, typically 20ms, is set
by the **blocksize** parameter in the [global] section of the *radiod*
configuration file; it applies to every receiver channel in that
*radiod* instance. The energy measurement is converted to an average
power, with 0 dBFS (decibels full scale) corresponding to a full scale
sine wave at the receiver output. (A full scale square wave would thus
be +3 dBFS.)

This avoids exaggerated responses to noise impulses
while still responding quickly to true signal level changes (20 ms
corresponds to a 50 Hz rate).

Except for pure tones, communication signals, including speech, are
noise-like with Gaussian statistics and a theoretically infinite
peak-to-average ratio. Occasional clipping is inevitable but rare
when the average amplitude is about -15 dBFS, so that's the default
target amplitude set by the **headroom** parameter.

It's tiring to listen to an empty channel with noise as loud as the
desired signal. A threshold feature prevents this. It is similar to
the AGC threshold settings in other receivers except that *ka9q-radio*
automatically estimates the noise floor and adjusts the threshold
accordingly. The energy in each frequency bin in and near the passband
is averaged over time, with the energy in the lowest bin taken to be
the noise floor.  It then limits gain to keep the noise at the output
below **headroom** minus **threshold**.

For example, with the defaults of **headroom** = -15 dBFS and
**threshold** = -15 dB, the average output level on noise will be -30
dBFS. When a signal appears and gradually increases in amplitude, the
gain remains constant and the output level increases. When it hits -15
dBFS, the gain decreases to keep it there. This decreases the noise
level.  There is presently no "slope" mechanism to decrease gain
before the output reaches the **headroom** setting.

When gain is reduced and a signal disappears or decreases in
amplitude, a **hang-timer** is started. The default (which can be
overridden in */usr/local/share/ka9q-radio/modes.conf* or
*radiod@foo.conf*) is 1.1 seconds, a typical value slightly longer than
the usual pauses in SSB voice.  If during this timeout the signal
amplitude again hits or exceeds the **headroom** setting, the gain is
reduced and the timer is restarted. If this doesn't happen before the
timer expires, the gain gradually increases at the
**recovery-rate** (default +20 dB/sec) until either the noise rises to
**headroom** minus **threshold** or the output level reaches
**headroom**. This is essentially the same as a classic SSB AGC. The
smaller **hang-time** usually preferred for CW is set in
*modes.conf* for those modes. Since the carriers in AM or AME
remove the guesswork from gain setting, these modes set
**hang-time** = 0 and **recovery-rate** = +50 dB/sec.

Although the AGC runs once per block, it computes a gain
rate-of-change (X decibels per sample) that's applied to every sample
in the block. This avoids audible gain "stairstepping" that would
otherwise occur during a rapid change at block boundaries. I.e., gain
is a continuous function, though gain rate-of-change is discontinuous.

I have found that measuring signal amplitude as a per-block average
eliminates the need for an "attack rate" setting to limit the rate at
which gain decreases on a strong signal. Averaging avoids overreacting
to short noise impulses while still reacting quickly to true changes
in signal level. Also, the average block amplitude is measured and the
new AGC gain change rate computed before the sample amplitudes are
actually changed, so the AGC "sees into the future" a little. This
also improves performance over an AGC that can only react to a signal
as it's being heard.

As an aside, the 20 ms AGC cycle in *ka9q-radio* is roughly the same
as the reaction time (tens of milliseconds) of the human acoustic
reflex (see [Wikipedia](https://en.wikipedia.org/wiki/Acoustic_reflex)).
This might just be an interesting coincidence.

I will probably add a feature to increase gain more rapidly when the
audio output is very low. This will keep the receiver from staying
deaf for a long time while it recovers from a very loud signal.
Experimentation here is needed.

