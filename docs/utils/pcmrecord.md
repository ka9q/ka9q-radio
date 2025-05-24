# pcmrecord

`pcmrecord` records, streams, or launch commands with RTP streams as input.

```
pcmrecord [OPTIONS]... <MCAST_IP>
```

## Description

This program reads one or more RTP streams from a multicast group and either writes them into a file, streams (one of them) onto standard output, or invokes a command for each stream and pipes the RTP data into it. PCM streams are written as-is (except that big-endian PCM is converted to little-endian). Opus streams are placed in a standard Ogg container.

### CLI Options

**-c, --stdout, --catmode**

> write one stream to stdout, default false. If --ssrc is not specified, selects the first one found and ignores the rest.

**-d, --directory**

> directory root in which to write files.

**-e, --exec**: 

> execute the specified command for each stream and pipe to it.

Several macros expanded as shown when found in the arguments:
  - $$: insert a literal '$'
  - $c: number of channels (1 or 2)
  - $d: description string from the radiod front end
  - $f: encoding ("s16le", "s16be", "f32le", "opus", "none")
  - $h: receive frequency in decimal hertz
  - $k: receive frequency in decimal kilohertz
  - $m: receive frequency in decimal megahertz
  - $r: sample rate, integer Hz
  - $s: ssrc (unsigned decimal integer)

**-o, --source**

> select only one source from a multicast group.

**-f, --flush**

> flush after each received packet, default false. Increases Ogg container overhead; little need for this writing files.

**-j, --jt**

> use K1JT format file names, default false.

**-l, --locale**

> set locale. Default is `$LANG`.

**-m, --mintime, --minfiletime**

> minimum file duration, default 0.2 sec. Files shorter than this are deleted when closed.

**-r, --raw**

> don't write .wav header for PCM files, default false. Ignored with Opus streams (Ogg is needed to delimit frames in a stream).

**-s, --subdirs, --subdirectories**

> create subdirectories when writing files: ssrc/year/month/day/filename.

**-S, --ssrc**

> select one SSRC from multicast (recommended for --stdout).

**-t, --timeout**

> close file after idle period, default 20 sec.

**-L, --limit, --length, --lengthlimit**

> maximum file duration (in seconds), default 0 (unlimited). When new file is created, round down to previous start of interval and pad with silence (for JT decoding).

**-x, --max_length**

> maximum recording duration (in seconds), default 0 (unlimited). Don't pad the .wav file with silence. Exit when all files have reached max duration.

**-4, --ft4**

> same as **--jt --lengthlimit 7.5**.

**-8, --ft8**

> same as **--jt --lengthlimit 15**.

**-w, --wspr**

> same as **--jt --lengthlimit 120**.

**-v, --verbose**

> increase verbosity level, default 0.

**-V, --version**

> show version and exit.

## Examples

Obviously the first step is running `radiod` with the correct settings (frequency, demodulator) for whatever mode you want to receive.

Record all streams in **out** folder:

```
pcmrecord -d out <MCAST_IP>
```

Decode APRS with external program:

```
pcmrecord -e "direwolf -n 1 -r 24000 -b 16 -" <MCAST_IP>
```
