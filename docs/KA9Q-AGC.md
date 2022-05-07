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

Linear Demodulator AGC in *radiod*

The linear demodulator in *radiod* has a very different kind of AGC

(to be continued)
