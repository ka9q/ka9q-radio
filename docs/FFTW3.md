FFTW3 Tuning

Because it uses fast convolution for frequency mixing and filtering,
'radiod' makes very heavy use of MIT's FFTW3 (Fastest Fourier Transform
in the West) library. By far the single most cpu-intensive operation
in the entire package is the forward FFT in 'radiod'.

FFTW3 provides a 'wisdom' feature to find and remember the most
efficient way to perform specific transform types and sizes.  While
'radiod' will probably run in real time without it on faster x86
systems it is especially recommended on the Raspberry Pi 4. This
requires that you run FFTW3's 'fftwf-wisdom' utility with the actual
transform sizes needed by the parameters you use with the 'radiod'
program. This can take hours but is worth the improvement in
performance.

Until I can figure out how to do all this easily and automatically,
here's a suggested run:

$ time fftwf-wisdom -v -T 1 -o nwisdom rof500000 cof36480 cob1920 cob1200 cob960 cob800 cob600 cob480 cob320 cob300 cob200 cob160

This finds the best way to do the following transforms:

rof500000: Airspy R2, 20 Ms/s real, 20 ms (50 Hz) block time, overlap 5 (20%).

cof36480: Airspy HF+, 912 ks/s complex, 20 ms (50 Hz), overlap 2 (50%).

cob1920, etc: inverse FFTs for 20 ms (50 Hz) block times, overlap
factors of 2 and 5, and various common sample rates supported by the
Opus codec (8/12/16/24/48 kHz).

NB!! fftwf-wisdom isn't careful about permissions checking. Nor does it
do any checkpointing. I've had hour-long runs ruined because it
couldn't write its output file. Write into a temporary file (e.g.,
nwisdom) and then carefully move that into /etc/fftw/wisdomf after
backing up previous versions.

Note also that wisdom files computed for the multithreading option
(which I use) are NOT compatible with wisdom files computed without
multithreading. That's true even for -T 1 (multithreading with just
one thread). Do NOT omit the -T 1 option, or you may destroy all your
previous computation work!

As I said earlier, I really need to find an easy way to do all this
automatically...

