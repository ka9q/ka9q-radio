*FFTW3* Tuning
==============

Because it uses fast convolution for frequency mixing and filtering,
*radiod* makes very heavy use of [MIT's *FFTW3*](http://www.fftw.org) (Fastest Fourier Transform
in the West) library. By far the single most cpu-intensive operation
in the entire package is the forward FFT in *radiod*.

*FFTW3* provides a 'wisdom' feature to find and remember the most
efficient ways to perform specific transform types and sizes on your
specific CPU.  While *radiod* will probably run in real time without
it on faster x86 systems it may actually be necessary to run in real
time at higher sample rates on the Raspberry Pi 4. This requires that
you manually run *FFTW3*'s bundled *fftwf-wisdom* utility with the
actual transform sizes needed by the parameters you use with the
*radiod* program. This can take hours but is worth the improvement in
performance.

FFTW3 stores its 'wisdom' files in two places: a system-wide file,
**/etc/fftw/fftwf-wisdom**, and an application specific (i.e., *radiod*)
file, **/var/lib/ka9q-radio/wisdom**. *Radiod* first reads the system-wide
file and then the application specific file. If wisdom is
not already available for the transforms it needs, it will perform a
half-hearted search (to not slow startup too much) and store it in
**/var/lib/ka9q-radio/wisdom**, but I recommend manually generating a full-blown
system-wide wisdom file with *fftwf-wisdom*.

Until I can figure out how to do all this easily and automatically,
here's a suggested run:

$ cd /etc/fftw  
$ touch nwisdom # make sure you have write permissions - fftwf-wisdom doesn't check before doing all its work!  
$ time fftwf-wisdom -v -T 1 -o nwisdom rof3240000 rof1620000 rof500000 cof36480 cob1920 cob1200 cob960 cob800 cob600 cob480 cob320 cob300 cob200 cob160 cob150  
$ ls -l nwisdom wisdomf # check that nwisdom is larger than wisdomf  
$ cp -i nwisdom wisdomf

This finds the best way to do the following transforms:

**rof3240000**: RX888 MkII at 129.6 MHz real, 20 ms (50 Hz) block, overlap 5 (20%).

**rof1620000**: RX888 MkII at 64.8 MHz real, 20 ms (50 Hz) block, overlap 5 (20%).

**rof500000**: Airspy R2, 20 Ms/s real, 20 ms (50 Hz) block time, overlap 5 (20%).

**cof36480**: Airspy HF+, 912 ks/s complex, 20 ms (50 Hz), overlap 2 (50%).

**cob1920**, etc: inverse FFTs for 20 ms (50 Hz) block times, overlap
factors of 2 and 5, and various common sample rates supported by the
Opus codec (8/12/16/24/48 kHz).

Generating wisdom for the larger transforms can take hours so you
might want to omit them if you don't plan to use the relevant hardware
(e.g., **rof3240000** and **rof1620000** for the Rx888 MK II). If you do have
a Rx888 Mk II, we've encountered thermal problems at the full sample
rate (129.6 MHz) so we don't recommend that until the thermal problems
can be fixed.

Whenever *radiod* is running you can always determine the block sizes in
use with the *control* program. Look in the 'Filtering' window. The
'FFT in' entry gives the size of the forward FFT; whether it is
real-to-complex (*fftwf-wisdom* prefix **rof**) or complex-to-complex
(prefix **cof**) is denoted by the letter 'r' or 'c' to the
right.  The 'FFT out' line gives the inverse FFT; it is always
complex-to-complex (prefix **cob**).

Don't omit the backward transforms. Although small and fairly fast
even without wisdom, they are used by every receiver channel so they
can add up if you have a lot of channels. And computing wisdom for
them takes little time.

NB!! *fftwf-wisdom* isn't careful about permissions checking. Nor
does it do any checkpointing. I've had hour-long runs ruined because
it couldn't write its output file. The directory **/etc/fftw** is owned by group *radio*, and you should make yourself a member of that group with

$ sudo addgroup **youruserid** radio

**/etc/fftw** should have been created with group write permission, but check that.

Have *fftwf-wisdom* write into a temporary file (e.g., **nwisdom**)
and then carefully copy that into **/etc/fftw/wisdomf** after backing
up previous versions. You might also want to do just one large
transform at a time, copying its output into **/etc/fftw/wisdomf**,
and re-running with the next. *fftwf-wisdom* reads
**/etc/fftw/wisdomf** and merges that into its generated output.  Make
sure the output of *fftwf-wisdom* is larger than the previous
**wisdomf** before overwriting it!  I like to keep all the previous
**wisdomf** files just in case I need back up (**wisdomf.0**,
**wisdomf.1**, etc). Make sure you change the output file name for
each new run so you don't just overwrite the previous run's output
(yes, I've done it.)

The '-t' option (note, -t not -T) sets a time limit on
fftwf-wisdom in decimal hours. It is not checked until after completion of the current
transform, so you won't lose any work because of it. Because of the
lack of checkpointing I often use it to limit run time and how much
work I will lose if a single run aborts.

Note also that wisdom files computed for the multithreading option
(which I use) are *NOT* compatible with wisdom files computed without
multithreading. That's true even for -T 1 (multithreading with just
one thread). Do *NOT* omit the -T 1 option, or you may destroy all
your previous computation work! Again, keep all your previous wisdomf
files so you can back up if you make a mistake.

It's tempting, but *don't* just blindly copy **/etc/fftw/wisdomf** from
one machine to another. Generate a new one. I once saw *radiod* run at
only half speed on a Rasberry Pi 400 after I copied **wisdomf**
from a regular Pi 4. Apparently the different Broadcom SoC stepping
(C0 vs B0, I think) changed something in memory or cache layout. Just
go off and watch Youtube while you rebuild **/etc/fftw/wisdomf**.

Note also that wisdom files generated by one version of FFTW3 aren't
good on a different version.  Debian 11.0 (bullseye) uses FFTW version
3.3.8 while Debian 12.0 (bookworm) uses version 3.3.10. Regenerate
them.

I know, I know, all this *really*, *REALLY* needs to be
automated... Unfortunately, it looks like it will require digging into
the guts of at least *fftwf-wisdom*.

As mentioned above, *FFTW3* provides a way to automatically create and
cache wisdom at runtime, but with a very long startup delay. I need to
move wisdom generation to a separate process independent of
*radiod*. It would be nice if *FFTW3* automatically learned as it
went, getting faster on user data without performing the exhaustive
search up front.

A Note on the FFT
-----------------

The [Wikipedia article on the
FFT](https://en.wikipedia.org/wiki/Fast_Fourier_transform) is a pretty
comprehensive read, with many references.

The original paper by James W Cooley and John W Tukey describing the
FFT can be found here: [An Algorithm for the Machine Calculation of
Complex Fourier
Series](https://www.ams.org/journals/mcom/1965-19-090/S0025-5718-1965-0178586-1/S0025-5718-1965-0178586-1.pdf). It'd
be hard to find a more widely used numerical algorithm in the modern
world. Besides its traditional use in spectral analysis, the FFT is
used in many audio and image compression algorithms. It is also the
basis of orthogonal frequency division multiplex (OFDM), which has
pretty much taken over modern terrestrial digital communications. The FFT
is also heavily used in medical imaging, such as MRI, CT and PET
scanners.

Phil Karn, KA9Q
July 2023
