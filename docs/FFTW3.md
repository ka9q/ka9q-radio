*FFTW3* Tuning
==============

Because it uses fast convolution for frequency mixing and filtering,
*radiod* makes very heavy use of MIT's *FFTW3* (Fastest Fourier Transform
in the West) library. By far the single most cpu-intensive operation
in the entire package is the forward FFT in *radiod*.

*FFTW3* provides a 'wisdom' feature to find and remember the most
efficient way to perform specific transform types and sizes.  While
*radiod* will probably run in real time without it on faster x86
systems it is especially recommended on the Raspberry Pi 4. This
requires that you run *FFTW3*'s **fftwf-wisdom** utility with the actual
transform sizes needed by the parameters you use with the *radiod*
program. This can take hours but is worth the improvement in
performance.

Until I can figure out how to do all this easily and automatically,
here's a suggested run:

$ time fftwf-wisdom -v -T 1 -o nwisdom rof3240000 rof1620000 rof500000 cof36480 cob1920 cob1200 cob960 cob800 cob600 cob480 cob320 cob300 cob200 cob160 cob150

This finds the best way to do the following transforms:

rof3240000: RX888 MkII at 129.6 MHz real, 20 ms (50 Hz) block, overlap 5 (20%).

rof1620000: RX888 MkII at 64.8 MHz real, 20 ms (50 Hz) block, overlap 5 (20%).

rof500000: Airspy R2, 20 Ms/s real, 20 ms (50 Hz) block time, overlap 5 (20%).

cof36480: Airspy HF+, 912 ks/s complex, 20 ms (50 Hz), overlap 2 (50%).

cob1920, etc: inverse FFTs for 20 ms (50 Hz) block times, overlap
factors of 2 and 5, and various common sample rates supported by the
Opus codec (8/12/16/24/48 kHz).

You might want to omit the larger transforms if you don't plan to use
the relevant hardware (e.g., rof3240000 and rof1620000 if you don't
have a Rx888 MK II). If you do have a Rx888 Mk II, we've encountered
thermal problems at the full sample rate (129.6 MHz) so we don't
recommend that until the thermal problems can be fixed.

While radiod is running you can determine the block sizes in use with
the 'control' program. Look in the 'Filtering' window. The 'FFT in'
entry gives the size of the forward FFT; whether it is real-to-complex
(fftwf-wisdom prefix rof) or complex-to-complex (fftwf-wisdom prefix
cof) is denoted by the letter 'r' or 'c' to the right.  The 'FFT out'
line gives the inverse FFT; it is always complex-to-complex
(fftwf-wisdom prefix cob).

NB!! **fftwf-wisdom** isn't careful about permissions checking. Nor
does it do any checkpointing. I've had hour-long runs ruined because
it couldn't write its output file. Write into a temporary file (e.g.,
nwisdom) and then carefully move that into /etc/fftw/wisdomf after
backing up previous versions. You might also want to divide up the
runs for large transforms by doing just one transform at a time,
merging its output into the system wisdom file /etc/fftw/wisdomf, and
re-running with another transform. Make sure the new wisdomf file is
both non-empty and larger than the previous wisdomf before overwriting
it!  I like to keep all the previous wisdomf files just in case I need
back up (wisdomf.0, wisdomf.1, etc). Make sure you change the output
file name for each new run so you don't just overwrite the previous
run's output (yes, I've done it.)

The '-t' option (note, -t not -T) sets a time limit on
fftwf-wisdom. It is not checked until after completion of the current
transform, so you won't lose any work because of it. Because of the
lack of checkpointing I often use it to limit run time and how much
work I will lose if a single run aborts.

Note also that wisdom files computed for the multithreading option
(which I use) are *NOT* compatible with wisdom files computed without
multithreading. That's true even for -T 1 (multithreading with just
one thread). Do *NOT* omit the -T 1 option, or you may destroy all
your previous computation work! Again, keep all your previous wisdomf
files so you can back up if you make a mistake.

Yes, all this *really* needs to be automated... Unfortunately, it
looks like it will require digging into the guts of at least
fftwf-wisdom.

*FFTW3* provides a way to automatically create and cache wisdom at
runtime, but with a very long startup delay. I need to move wisdom
generation to a separate process independent of *radiod*. It would be
nice if *FFTW3* automatically learned as it went, getting faster on
user data without performing the exhaustive search up front.

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
pretty much taken over modern terrestrial digital communications. It
is also heavily used in medical imaging, such as MRI, CT and PET
scanners.

Phil Karn, KA9Q
July 2023
