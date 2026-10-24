# Editor's notes

These drafts describe the code inspected, including some behavior that differs from comments or usage output. The following points deserve your attention before publication. They are documentation review notes, not changes made to the software.

## Important behavior differences

1. **`pcmrecord -x` and `-L`:** both cases now assign `Max_length`. The old usage line still describes `-x` as a distinct one-shot mode. The draft documents the shared handler and does not promise a one-shot exit. `wd-record` retains separate `FileLengthLimit` and `max_length` variables and its completion/exit logic; its page intentionally differs.
2. **`radiod` options:** actual parser is `N:vVb::d::s::`. USB selectors have optional attached arguments, with no separating space. The usage line lists `-h`, but there is no successful help handler and no long-option parser. The draft avoids inventing `--help` or `--version`.
3. **Configuration selection:** `loadconfig()` chooses an existing regular file, an explicitly supplied directory, or the appended `.d` fallback when the original path is absent. It does not merge a regular file and sibling directory. `blocktime` is seconds in current source, default `.02`.
4. **`tune` query side effects:** every request sends `LIFETIME`, whose default is zero. A plain query can therefore change lifetime. It retries at up to ten commands/s, uses a new tag on retransmission, and has no overall deadline. Zero-valued sweep settings are not sent, so those options cannot currently clear a sweep field to zero. The long `--lifetime` option works, but its advertised short `-t` is absent from `Optstring`; the draft lists the long form only.
5. **`monitor --center`:** long-option table maps it to `C`, but the second-pass handler has no `case 'C'`. The INI `center` key does work. The page identifies the difference instead of advertising a working CLI feature. `--update` is confirmed to be **milliseconds**, default 100, from `monitor-display.c` and its curses timeout call.
6. **`monitor --pipe`:** Linux output is headerless native-endian `int16_t` PCM at the configured playback rate and channel count. The comment says 48 kHz stereo, but the loop actually uses the variables. This branch's source comment says it was not yet tested; runtime verification is advisable.
7. **`metadump --all`:** sets `Ssrc` to `0xffffffff`, while the input thread rejects packets with a different SSRC. This appears to suppress the individual-channel replies that all-channel polls elicit. It can count/display its own command packets. The draft points readers toward a specific SSRC or passive observation. Confirm intended behavior before keeping this limitation in the published page.
8. **`metadump` parsing quirks:** interval casts to `int64_t` before multiplying by `BILLION`, dropping fractions. `LANG` is read after `--locale` and overrides it. Passive mode still uses a finite accepted-packet count (default two), although it can wait indefinitely if packets do not arrive.
9. **`fft-gen`:** usage advertises `--destroy-input`/`-d`, but neither is parsed. The `d` position in a transform token does implement out-of-place destructive input. The writer uses version-specific application wisdom files and merges prior file wisdom even in force mode. Main currently exits zero on several error paths; the page advises checking diagnostics.
10. **`powers --overlap`:** the long option returns `O`, absent from the switch and short option string. It is not currently usable. Extra `t`/`i` entries in option strings are not evidence of implemented public features. The draft covers the working handlers only and identifies the unused long option.
11. **`packetd --version`:** the long-option declaration requires an argument, while `-V` does not. The draft recommends `-V` and records the discrepancy. Optional status-input discovery deserves a runtime example; its interactions with explicit PCM groups are complex.
12. **`pcmsend` and `opussend`:** despite some usage text calling `-I` an input multicast address, it selects a **PortAudio capture device**. Both currently fall back to `Pa_GetDefaultOutputDevice()` when no device is selected. They lack current status metadata; `opussend.c` explicitly warns it may no longer work with current consumers. The drafts use explicit capture selection and label the compatibility limitation. A `case 'p'` exists in `pcmsend`, but `p` is absent from its getopt string, so no `-p` is advertised there.
13. **`aprs`:** source comments claiming no rotor control are stale: it connects and emits `\\set_pos` commands. Destination parsing copies the host string before removing `:port`, and can also modify a default string literal. The draft marks the rotor path experimental and does not supply a supposedly verified rotor command. Observer coordinates still default to KA9Q's location; the page makes those explicit.
14. **`cwd`:** usage says `level16`, but the code passes the setting to `init_morse()`, which interprets a negative-absolute dB amplitude level. The draft gives the implementation and default -29 dB, not raw 16-bit units. No version flag is implemented.
15. **`ctcss`:** long version option is case-sensitive `--Version`. Current processing assumes big-endian 16-bit PCM; accepting arbitrary RTP formats would overpromise. The page describes CTCSS output without advertising the experimental DTMF code as a supported interface.
16. **`jt-decoded`:** derives decoder base frequency from SSRC, rather than channel metadata. `-x` changes FT4/FT8 decoder paths only; its usage mentions JT9 but implementation invokes `decode_ft8`. Defaults to WSPR. It is distinct from the newer independent recording/decoding service arrangement.
17. **`wd-record`:** advanced BPSK synchronization, injected errors, folding, and timing controls are source-derived descriptions, not a validated operational guide. In `-W` processing, if no `-L` limit has been set **yet**, 60 seconds is installed. Review argument-order effects and finite-recording behavior. Its input-stream behavior should be tested with the actual WSPRdaemon deployment.

## Editorial choices

- These replace the 17 pages currently installed by `docs/Makefile`. They do not add unlisted utility or internal-library pages.
- They use portable `man(7)` macros and editable roff, without a documentation-framework dependency.
- Version flags are included only where implemented. Help behavior is not assumed to be uniform across programs.
- Paths distinguish local `/usr/local` installations from packaged `/usr` installations where needed.
- Examples are intended to illustrate valid syntax, not prove a working runtime setup. No network or device actions were executed.
- No blanket exit-code scheme is invented. In particular a matching command tag is not described as confirmation that every requested parameter was applied.
- Long option aliases were checked against their tables and handlers. Defects are described under notes/limitations rather than silently repaired in documentation.
- The current command/parameter spreadsheet and PDF remain the intended registry; these pages do not recreate the deleted `proto.md`.

## Suggested review order

Start with `radiod.8`, `tune.1`, `pcmrecord.1`, and `monitor.1`. Those cover the main user journey. Then check `fft-gen.1`, `metadump.1`, and `powers.1`. Review integration and legacy utilities with their maintainers or deployment users, particularly `wd-record` and the audio senders.

When you choose to fix an implementation discrepancy above, update the corresponding draft at the same time. The limitations are a snapshot of the source inspected, not intended permanent requirements.
