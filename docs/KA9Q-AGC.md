Automatic Gain Control (AGC) in *ka9q-radio*

Front End AGC

The *ka9q-radio* package actually has two independent AGC mechanisms
serving very different purposes. The first AGC is in the front end
handler (e.g., *airspyd*) to control the signal level at the A/D
converter input in the SDR front end hardware. When the signal exceeds
an upper threshold, the analog gain ahead of the A/D converter is
reduced in steps; when it falls below a lower threshold, it is
increased.

SDR tuners typically have three configurable analog gain stages: LNA,
mixer and IF (or baseband).  Some front ends have a switchable bias tee
to power an external preamp.  This is controlled in the config file
and is not part of the first AGC.

Proper gain distribution is essential to optimize overall sensitivity
and linearity (i.e., susceptibility to intermodulation
distortion). The vendor-supplied library for the Airspy R2 comes with
separate gain tables that optimize either sensitivity or linearity,
but most other SDR front ends do not. These tables are optimum only
without an external preamp.

The analog gains are passed in the metadata stream to the *radiod*
module where the signal is digitally attenuated by an equal amount to
maintain a constant overall gain from the antenna terminal. This keeps
the second AGC (in radiod's linear demodulator) from seeing abrupt
level changes when the first AGC changes.  This is only partly
effective mainly because the analog gains are not well calibrated, but
also because of small uncontrolled timing skews between the metadata
and signal paths.  But it works well in practice. The antenna signal
is the sum of many transmitters and noise sources with fairly
stationary (i.e, stable) Gaussian statistics. It only changes
significantly when a strong nearby transmitter keys on or off, and
that's the reason for the hysteresis mentioned earlier.

The Linear Demodulator AGC in *radiod*

This AGC is similar to those in traditional communications
receivers. It operates after downconversion and filtering so it
responds only to in-band signals.

The AGC is controlled by signal energy measurements made on each block
of samples from the filter.  The block length, typically 20ms, is set
by the **blocksize** parameter in the [global] section of the *radiod*
configuration file. The energy measurement is converted to an average
power, with 0 dBFS (decibels full scale) corresponding to a full scale
peak sine wave at the receiver output.

This averaging avoids exaggerated responses to brief impulse noise
while still responding quickly to true signal level changes (20 ms
corresponds to a 50 Hz rate).

Except for pure tones, communication signals are noise-like and have
Gaussian statistics. Gaussian noise has a theoretically infinite
peak-to-average ratio so it is impossible to totally avoid clipping.
But it's rare when the average amplitude is about -15 dBFS, so that's
the default target amplitude set by the **headroom** parameter.

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
overridden in */usr/local/share/ka9q-radio/modes.conf*) or in
*radiod@foo.conf* is 1.1 seconds, a typical value slightly longer than
the usual pauses in SSB voice.  If during this timeout the signal
amplitude again hits or exceeds the **headroom** setting, the gain is
reduced and the timer is restarted. If this doesn't happen before the
timer expires, the gain is gradually increased at the
**recovery-rate** (default +20 dB/sec) until either the noise rises to
**headroom** minus **threshold** or the output level reaches
**headroom**. This is essentially the same as a classic SSB AGC. The
smaller **hang-time**s usually preferred for CW are set in
*modes.conf* for those modes. Since the constant carriers in AM or AME
remove the "guesswork" from gain setting, so these modes set
**hang-time** = 0 and **recovery-rate** = +50 dB/sec.

Although the AGC parameters are recomputed on a block basis, it
actually computes a gain rate-of-change that is applied to every
sample in the block. This avoids audible gain "stairstepping" that
would otherwise occur during a rapid change at block boundaries.

I have found that measuring signal amplitude as a per-block average
eliminates the need for an "attack rate" setting to limit the rate at
which gain decreases on a strong signal. Averaging eliminates the
overreaction to short noise impulses that might otherwise occur while still
reacting quickly to true changes in signal level.

(to be continued)


