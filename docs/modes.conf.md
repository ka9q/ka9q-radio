*modes.conf* summary
====================

The following modes (presets) are defined in the current version of */usr/local/share/ka9q-radio/modes.conf*. See the file itself for complete details

*pm* - Phase modulation, i.e, standard amateur narrowband FM, 16 kHz
bandwidth, 6 dB/octave de-emphasis.

*npm* - Narrow PM. Like *pm* but a 12.5 kHz bandwidth.

*fm* - Frequency modulation. Like *pm* but without
de-emphasis. Sometimes called "true FM". Recommended for digital modes
such as packet, DMR, etc. Use *pm* or *npm* for conventional so-called
"NBFM" voice.

*nfm* - Narrow FM. Same as *fm* but 12.5 kHz bandwidth.

*wfm* - Wideband FM, for standard FM stereo broadcasting. Output
forced to 48 kHz stereo.

*am* - Envelope detected amplitude modulation.

*cam* - Coherent double sideband amplitude modulation, aka "synchronous" AM.

*ame* - "Enhanced" AM. Like *cam* except upper sideband only (e.g., for CHU).

*iq* - I/Q (complex) baseband output for digital demodulators or other processing.

*cwu* - CW, upper sideband. Same tone pitch as *usb* but with narrow filter centered at 500 Hz.

*cwl* - CW, lower sideband. Same tone pitch as *lsb* but with narrow filter centered at 500 Hz.

*usb* - Upper sideband. DSP filtering permits good low frequency response.

*lsb* - Lower sideband.

