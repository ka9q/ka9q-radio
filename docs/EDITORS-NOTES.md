# Editor's notes

Updated 26 October 2026 against commit `5a71f08ed7d8e052504a8c6bcb18233b0df79947` (`cleanup command args and man pages`). The source and manual pages in the repository are the working versions; the original generated drafts are historical. This replaces the 23 October review.

This pass compared all 17 installed manual pages with their option tables and relevant handlers, reviewed the changes since `af940c5c`, and checked the new SSRC-zero frontend-status path. It is a source/documentation review, not a runtime validation. No source or manual-page changes were made as part of updating these notes.

## First: two regressions in status encoding

These deserve attention before further manual editing because they affect existing channel replies as well as the new frontend-only replies.

### RF gain's condition now controls the LO-frequency field

At `src/radio_status.c:723`, the code currently reads:

```c
if(!isnan(frontend->rf_gain) && isfinite(frontend->rf_gain))
encode_double(&bp,FIRST_LO_FREQUENCY,frontend->frequency);
  encode_float(&bp,RF_GAIN,frontend->rf_gain);
```

Only the immediately following statement belongs to the `if`: the first LO frequency is omitted when RF gain is unavailable, while RF gain is passed to the encoder unconditionally. This looks like a misplaced insertion during the frontend/channel split. Emit `FIRST_LO_FREQUENCY` independently, and retain the validity condition around `RF_GAIN`. The float encoder already omits NaNs, but that does not restore the missing LO field or the intended infinity check.

### Noise-density validity condition has become part of a comment

At `src/radio_status.c:766`:

```c
// Adjust for A/D wid  if(!isnan(chan->sig.n0) && isfinite(chan->sig.n0) && chan->sig.n0 > 0)
  encode_float(&bp,NOISE_DENSITY,power2dB(chan->sig.n0));
```

The previous finite-and-positive test is now commented out. Restore the condition before converting to dB. Omitting NaNs in `encode_float()` does not make the earlier dB conversion conditional, and zero or infinite values no longer have the previous field-omission behavior.

## Documentation that now lags the fixed code

| Page | What to update | Current source evidence |
|---|---|---|
| `tune.1` | Remove the claim that queries send lifetime zero. An unspecified lifetime is now omitted; explicitly setting zero still requests no expiry. A received query still refreshes an existing lifetime through the command handler. | `src/tune.c:51,288`; `src/radio_status.c:144` |
| `tune.1` | Document `-t frames` alongside `--lifetime frames`; remove the missing-short-option warning. | `src/tune.c:59` |
| `tune.1` | Remove both claims that zero-valued sweep options are omitted/cannot clear a setting. The client now sends explicitly supplied zeros using NaN sentinels. Server validation still determines whether each requested value is accepted. | `src/tune.c:44,332`; server sweep handlers in `src/radio_status.c` |
| `monitor.1` | Add `-C, --center`: disable automatic stereo positioning. The obsolete limitation has already been removed, but the working option is not yet listed. | `src/monitor.c:284` |
| `pcmsend.1` | Add `-p value` for IP type of service, parsed with base zero; default 48. The getopt omission is fixed. | `src/pcmsend.c:76,81` |
| `packetd.8` | Document `--version` alongside `-V`. Its incorrect-argument warning is already gone and the implementation now accepts the long form without an argument. | `src/packetd.c:107` |
| `powers.1` | Replace the stale statement that `--overlap` appears in the option table but is unhandled. It has been removed entirely; overlap is fixed at zero in this utility. | `src/powers.c:35,36,73` |
| `metadump.1` | Add explicit `--ssrc 0` as the frontend-only status query. Omitted `--ssrc` means passive listening; explicit zero no longer means the same thing. | `src/metadump.c:102,195`; `src/radio_status.c:49` |
| `control.1` | Add `-s 0` for the new frontend-only view. Clarify that this status path currently ignores parameter-change commands rather than controlling an ordinary receiver channel. | `src/control.c:433,451,582`; `src/radio_status.c:49` |
| `control.1` | Distinguish local preset-menu definitions from server-side execution. The UI loads a local `presets.conf` to build its choices, while radiod applies the named preset using its own definitions. The current FILES wording identifies only the server file. | `src/control.c:48,572,576`; preset handler in `src/radio_status.c` |

The following source/doc pairs are already reconciled: fractional `metadump --interval`, removal of `fft-gen --destroy-input` from usage and its stale manual warning, `cwd`'s dB-level usage label, the `opussend -I` usage label, and the installed section-8 uninstall directory.

## New frontend-only status: specify its contract

The server now interprets SSRC zero as a request for frontend status, and both `metadump` and `control` distinguish explicit zero from no selection. This is useful, but needs an explicit API paragraph and client examples.

- `metadump -n -s 0 RADIO` requests the frontend-only response.
- `control -s 0 RADIO` selects that view without invoking channel selection.
- `tune` still rejects SSRC zero (`src/tune.c:204`). Do not imply frontend polling is supported uniformly by all clients.
- The server's SSRC-zero branch does not run `decode_radio_commands()`. Parameter TLVs in these packets are ignored, including requests affecting the frontend.
- The frontend-only response currently omits `COMMAND_TAG`, because tag encoding is after the `chan == NULL` branch (`src/radio_status.c:755`). The API guide's universal promise that a received command's tag is echoed therefore needs an exception, or the frontend poll needs its own tag-handling implementation. `metadump` does not require a matching tag, so it can display the reply as written.
- Frontend-only replies use `Output_fd` unconditionally (`src/radio_status.c:125`), whereas ordinary channels select the TTL-zero or nonzero socket. Verify intended behavior in local-only and LAN configurations; send errors in the frontend-only case are currently not logged by that function.
- The first call to `encode_radio_status()` in the SSRC-zero branch (`src/radio_status.c:51`) writes a result that is discarded; `send_radio_status()` immediately encodes again. It appears redundant.

Keep the distinction between status and control clear: the current implementation adds frontend observation, not a complete standalone frontend-control channel.

## Remaining implementation questions

### `metadump --all` still filters out the replies it requests

`--all` sets `Ssrc = 0xffffffff` and `Ssrc_set = true` (`src/metadump.c:135`). The input thread then requires an exact SSRC match (`:263`). Actual individual-channel replies have their own SSRCs, so they are discarded. Its own all-channel command packets can still be displayed and counted.

The existing man-page limitation remains accurate. The new explicit-selection flag fixes `-s 0`, not `--all`. Separate the all-channel receive filter from single-SSRC filtering, then remove the warning when confirmed.

The `--all` interval clamp still uses `min(1 second, Interval)` (`:138`) even though its comment says no more than one poll per second. This caps intervals at one second rather than enforcing a one-second minimum. Resolve the intended behavior before documenting a rate limit.

### `metadump` locale precedence remains

`LANG` is read after option parsing (`src/metadump.c:147`) and overrides `--locale` when present. The man page already describes this. The fractional-interval cast is fixed and no longer belongs in the unresolved list.

Packet count remains a count of accepted commands and status packets, not just replies. Passive mode defaults to two accepted packets and can wait indefinitely if none arrive. These are behavior notes, not newly discovered defects.

### Audio capture senders still select the default output-device index

Both `pcmsend` and `opussend` still call `Pa_GetDefaultOutputDevice()` when no `-I` device is supplied, then open that index as a capture device (`src/pcmsend.c:129`, `src/opussend.c:164`). Consider `Pa_GetDefaultInputDevice()` and appropriate device validation. The present pages correctly recommend explicit input selection.

The lack of current channel-status metadata remains an interoperability limitation; the existing `opussend.c` header still cautions that it may no longer work with current consumers. Correcting usage wording did not change that transport behavior.

### `aprs` rotor destination parsing remains problematic

The obsolete “doesn't yet drive antenna rotors” comment is removed. The actual code still connects and emits rotor position commands. Remaining issues at `src/aprs.c:119`:

- The resolver hostname is copied before `:port` is split from `Dest`; the copied hostname can still contain the port.
- Splitting the default destination modifies a string literal.
- Resolver hints specify UDP/datagram, but the subsequent connection opens a TCP/stream socket. Decide which transport is intended and make resolution and connection agree.

Keep its experimental warning until that path is verified. Explicit observer coordinates and selected APRS source remain important for use with a physical rotator.

### `fft-gen` exit status still needs care

The stale `--destroy-input` help entry is removed. Destructive-input selection remains the `d` letter in transform tokens. Planning/file errors can still reach a zero exit status (`src/fft-gen.c`, main/`bomb` path); the manual warning remains appropriate for scripts. Force mode's merging of existing saved wisdom also remains as previously described.

### Intentional or deployment-specific behavior to retain

- `tune` has no overall response deadline and may keep retrying. Fixing the lifetime/sweep omissions does not change that.
- `pcmrecord` still routes `-x` and `-L` to the same `Max_length` handler. Its page intentionally avoids promising separate one-shot semantics. `wd-record` still has separate finite-limit completion behavior.
- `ctcss` still has case-sensitive `--Version` and assumes big-endian 16-bit PCM in its sample loop. The page matches that implementation; lowercase normalization is optional.
- `cwd` still has no version flag. Its level is a negative-absolute dB amplitude value, now reflected in usage.
- `jt-decoded` still derives decoder base frequency from SSRC, defaults to WSPR, and uses `-x` only for FT4/FT8 decoder executables. Its usage still refers to JT9 rather than the implemented `decode_ft8` invocation.
- `wd-record` synchronization, forced-error, and folding controls still need deployment-level validation. `-W` installs 60 seconds only if no `-L` limit has been set yet; argument order remains relevant.
- `monitor --pipe` continues to write native-endian 16-bit PCM at configured rate/channel count. The “not yet tested” source comment is gone; this review does not infer runtime validation from that edit.

## Resolved since the original review

| Original item | Current disposition |
|---|---|
| Phantom `radiod -h` usage entry | Usage line removed; no successful help option is promised in the page. A genuine help handler would be a separate enhancement. |
| `tune` always sending lifetime zero | Fixed: lifetime omitted unless explicitly supplied. |
| Missing `tune -t` | Fixed in getopt string. |
| Zero-valued sweep settings omitted | Fixed using NaN sentinels. |
| Missing `monitor --center` handler | Fixed with `case 'C'`. Add the option to the page. |
| Incorrect monitor update unit | Source usage and page now say milliseconds. |
| Fractional metadump interval truncated | Fixed: multiplication is inside the cast. |
| Missing explicit frontend-only selection | New `Ssrc_set` handling in metadump/control supports zero. Document the new contract above. |
| Advertised but unparsed `fft-gen --destroy-input` | Removed from usage; stale manual note removed. |
| Advertised but unparsed `powers --overlap` | Removed from option table; stale manual note remains. |
| `packetd --version` requiring an argument | Fixed. |
| Unreachable `pcmsend -p` handler | Fixed by adding `p:` to getopt string. |
| Misleading `opussend -I` usage | Corrected to input device. |
| Obsolete APRS no-rotor comment | Removed; destination issues remain separately listed. |
| `cwd` usage suggesting raw integer level | Changed to dB. |
| Section-8 uninstall using man1 directory | Fixed to man8 in `docs/Makefile`. |

Configuration-file selection, seconds-based `blocktime`, and the original distinction between shared preset parameters and actual channel state remain valid background facts, not open option-parser bugs.

## Formatting and generated material

`mandoc -Tlint` on the current 17 installed pages reported only five STYLE messages for source lines longer than 80 bytes: three in `control.1` and two in `monitor.1`. No structural rendering warnings/errors were reported. Literal `*not*` in roff text is not Markdown emphasis; use a roff font escape if emphasis is intended.

`docs/PREVIEW.txt` and `docs/PREVIEW.html` are still the original generated previews and do not reflect the current man-page edits. Regenerate them when the next editing round is complete, or label them historical. The old `VALIDATION.json` describes the original drafts rather than this revision.

The Numbers command registry and its regenerated PDF remain the maintained field reference. The deleted `proto.md` is not part of this review. Do not regenerate or restore old draft pages over the edited files.

## Suggested next pass

1. Repair the two status-encoder regressions.
2. Update `tune.1`, list the newly working CLI options, and document SSRC-zero status behavior.
3. Resolve `metadump --all` and its polling-interval clamp.
4. Verify the older sender/rotor paths and the WSPRdaemon-specific recorder in their actual deployments.
5. Regenerate previews after the manual text settles.
