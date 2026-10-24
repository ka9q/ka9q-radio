# ka9q-radio man-page drafts

17 editable `man(7)`/roff files replacing the pages listed by the current `docs/Makefile`. Prepared 23 October 2026 from the local source checkout rooted at `/Users/karn/src/ka9q-radio`. HEAD was `af940c5c9c912ae34cf5608bf836c0671aec75fe`; the checkout also contained local changes. No project source or existing documentation was overwritten.

These are user-facing command references, not the proposed internal architecture manual. Syntax, option descriptions, defaults, and limitations were checked against source handlers rather than copied from the existing template pages. Runtime examples have **not** been executed.

## Main receiver and client pages

| Draft | Purpose |
|---|---|
| [radiod.8](radiod.8) | Receiver server, configuration selection, device options, signals, service operation |
| [tune.1](tune.1) | Scriptable channel configuration, presets, output settings, sweeps |
| [control.1](control.1) | Interactive channel control and discovery |
| [monitor.1](monitor.1) | Audio playback, multiple sessions, playout, voting, pipe output |
| [pcmrecord.1](pcmrecord.1) | Files, stdout, per-stream commands, segmentation and reacquisition |
| [metadump.1](metadump.1) | Packet decoding, status polling, passive observation |
| [powers.1](powers.1) | Spectrum requests and textual power output |
| [fft-gen.1](fft-gen.1) | FFTW planning, transform specification, wisdom files |

## Additional installed pages

| Draft | Purpose |
|---|---|
| [packetd.8](packetd.8) | AFSK/AX.25 demodulation |
| [aprsfeed.8](aprsfeed.8) | Forward APRS traffic to APRS-IS |
| [aprs.1](aprs.1) | Position/pointing display and rotor-control code |
| [ctcss.1](ctcss.1) | Tone measurements |
| [cwd.1](cwd.1) | Morse audio from a FIFO |
| [pcmsend.1](pcmsend.1) | Local audio capture to RTP PCM |
| [opussend.1](opussend.1) | Local audio capture to RTP Opus |
| [jt-decoded.1](jt-decoded.1) | Legacy combined recorder/weak-signal decoder |
| [wd-record.1](wd-record.1) | WSPRdaemon recorder and synchronization options |

## Reviewing and editing

Edit the `.1` and `.8` files directly. [PREVIEW.txt](PREVIEW.txt) contains a rendered, searchable copy of every page; regenerate it after editing if desired. [EDITORS-NOTES.md](EDITORS-NOTES.md) records implementation discrepancies and review points.

On Linux, preview a local draft without installing it:

```sh
man -l ./radiod.8
man -l ./pcmrecord.1
```

Or render with a formatter:

```sh
mandoc -Tutf8 ./radiod.8 | col -b
groff -man -Tutf8 ./radiod.8 | col -b
```

Formatting checks used for these drafts:

```sh
mandoc -Tlint ./radiod.8
groff -man -Tutf8 ./radiod.8 > /dev/null
```

After editing, copy the desired `.1`/`.8` files into the repository's `docs/` directory. Existing build/install lists already name these 17 files. No Makefile change is needed just to replace their contents. `pcmrecord.1` and `cwd.1` are section 1, matching the current install list, even though some existing headers say otherwise.

Do not install the preview or editor notes as man pages. Draft examples use illustrative multicast names and directories; replace them with your configured destinations and existing writable directories.
