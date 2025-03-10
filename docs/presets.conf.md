# *presets.conf* Summary

The following presets are defined in the current version of */usr/local/share/ka9q-radio/presets.conf*.

See the file itself for complete details.

You may add or delete entries in this file as needed, but be careful as it will be overwritten by the next `make install`.

| name      | demod     | BW [Hz] | description |
|-----------|-----------|---------|-------------|
| pm        | fm        |  16.0k  | amateur radio narrowband FM, 6dB/octave de-emphasis, use for voice modes |
| npm       | fm        |  12.5k  | same as above |
| fm        | fm        |  16.0k  | amateur radio "true" FM, no de-emphasis, use for data modes |
| nfm       | fm        |  12.5k  | same as above |
| wfm       | wfm       | 220.0k  | commercial broadcast FM, with North America de-emphasisis, audioforced to 48k, stereo |
| am        | linear    |  10.0k  | envelope detected AM, passes DC |
| sam       | linear    |  10.0k  | coherent AM with carrier tracking |
| ame       | linear    |   5.1k  | coherent AM with carrier tracking, USB only |
| iq        | linear    |  10.0k  | raw I/Q output, usr for extra processing |
| cwu       | linear    |   0.4k  | CW on USB voice bands, same tone pitch as USB but with narrow filter centered at 500 Hz |
| cwl       | linear    |   0.4k  | CW on LSB voice bands, same tone pitch as LSB but with narrow filter centered at 500 Hz ||
| usb       | linear    |   3.05k | amateur radio USB |
| lsb       | linear    |   3.05k | amateur radio LSB |
| dsb       | linear    |  10.0k  | amateur radio DSB-SC, uses PLL squaring to recover carrier |
| amsq      | linear    |   6.0k  | envelope detected AM with carrier squelch |
| wspr      | linear    |   3.05k | same as **usb** but with AGC disabled |
| spectrum  | spectrum  |   NA    | experimental |
| nam       | linear    |   6.0k  | narrow (completely flat) AM, passes DC|
