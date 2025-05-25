# control

`control` is a utility integrated in **ka9q-radio** SW for run-time configuration of `radiod`.

## Description

### Runtime Commands

| keybind                       | description  |
|-------------------------------|--------------|
| q                             | quit program |
| l                             | lock/unlock frequency
| ⭾ (tab), ⇟ (page down)        | move cursor down
| SHIFT + ⭾ (tab), ⇞ (page up)  | move cursor up
| HOME                          | move cursor to top
| END                           | move cursor to bottom
| ← (arrow left), BKSP          | move cursor left one digit
| → (arrow right)               | move cursor right one digit
| ↑ Up arrow                    | increase by 1
| ↓ Down arrow                  | decrease by 1
| CTRL + L                      | redraw screen
| A                             | set front end (RF) attenuation, dB
| b                             | set Opus audio codec bit rate, b/s (0 = auto)
| B                             | set output packet buffering 0-4 frames
| e                             | set output encoding (s16le, s16be, f32le, f16le, opus)
| f                             | set frequency (eg: 147m435, 10m0, 760k0, 14313000)
| F                             | set frames in filter2 block (0-4: 0 = disable filter2)
| g                             | set gain, dB
| G                             | set front end (RF) gain, dB
| H                             | set output headroom, dB
| k                             | set Kaiser window beta parameter
| L                             | set AGC threshold, dB
| m, p                          | preset demod mode
| o                             | set/clear option flag
| O                             | set/clear aux option flag
| P                             | PLL loop bandwidth, Hz
| r                             | set refresh rate, s
| R                             | set AGC recover rate, dB/s
| s                             | set squelch, dB
| S                             | set sample rate, Hz
| T                             | set AGC hang time, s
| u                             | set output status update interval, frames
