Configuring *ka9q-radio*
========================

v0.0 (incomplete), May 2022
Phil Karn, KA9Q
---------------

A Linux system can run several copies of the ka9q-radio radio daemon,
*radiod*, subject to resource limits (CPU and Ethernet capacity).

Each *radiod* instance has a configuration file, e.g.,
*/etc/radio/radiod@2m.conf*, where "2m" is the instance name.  Each
*radiod* instance reads a raw A/D sample stream from a front end
module, e.g., *airspyd*. More than one *radiod* instance can read from
the same front end, but it is most efficient to have just one instance
per front end handling all the channels.

Here is an excerpt from *radiod@2m.conf* on my system:

>[global]
>blocktime = 20
>overlap = 5
>input = 2m-vertical.local
>samprate = 24000
>data = 2m-pcm.local
>mode = pm
>status = 2m.local

>[2m FM]
>mode = pm
>freq = "145m800 144m490 145m200 145m825 145m990 144m310 144m325 144m340 144m355"

There must be exactly one [global] section. Parameters here apply to
the entire *radiod* instance. Some (not all) may be overridden in
subsequent sections. The parameters in this section are as follows:

**blocktime** Decimal, default 20 milliseconds, corresponding to a 50
Hz block rate. This is the duration of the forward FFT shared by all
receiver channels, so it cannot be overridden in a later section. The
actual block size is equal to the blocktime times the A/D sample
rate. E.g., the Airspy R2 has a 20 MHz (real) sample rate, so a 20 ms
blocktime corresponds to a block size of 400,000 samples. The actual
FFT block size is larger and depends on the **overlap** setting.

Larger values of **blocktime** permit sharper channel filters and are
more tolerant to CPU scheduling latencies but add latency and incur
greater CPU overhead. (The relative per-sample CPU cost of an FFT
increases with the square root of the FFT block size.)

**overlap** Integer, default 5. This controls the ratio of old to new
samples in each forward FFT. An overlap of 5 means that 1/5 of each
FFT input block consists of A/D samples from the previous block and
4/5 are new A/D samples. The overlap limits the length of the impulse
response of the filter in each receiver channel. For example, an
**overlap** of 5 combined with a **blocktime** of 20 milliseconds
gives an overlap of 20 / 5 = 4 milliseconds, which becomes the maximum
impulse duration of the per-channel filter. Smaller values of
**overlap** permit sharper filters at the expense of greater CPU
overhead since each FFT processes more old data.

Only certain values of **overlap** will work properly, so unless
you're feeling very adventurous stick with the default of 5 until I
write a more detailed discussion of the considerations. When operating
on HF, you might try an **overlap** of 2 for sharper filters, as the
lower A/D sample rates make the extra CPU cost acceptable.





