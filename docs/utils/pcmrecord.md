# pcmrecord

`pcmrecord` records, streams, or launch commands with RTP streams as input.

This program reads one or more RTP streams from a multicast group and either writes them into a file, streams (one of them) onto standard output, or invokes a command for each stream and pipes the RTP data into it. PCM streams are written as-is (except that big-endian PCM is converted to little-endian). Opus streams are placed in a standard Ogg container.

## CLI parameters

**--stdout | --catmode | -c**: write one stream to stdout. If --ssrc is not specified, selects the first one found and ignores the rest.

**--source \<source-specific name or address\>**

**--directory | -d \<directory\>**: directory root in which to write files.

**--exec | -e '\<command args ...\>'**: Execute the specified command for each stream and pipe to it.
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

**--flush | -f**: Flush after each received packet. Increases Ogg container overhead; little need for this writing files.

**--jt | -j**: Use K1JT format file names

**--locale | -l <locale>**: Set locale. Default is $LANG

**--mintime | --minfiletime | -m**: minimum file duration, in sec. Files shorter than this are deleted when closed

**--raw | -r**: Don't emit .WAV header for PCM files; ignored with Opus (Ogg is needed to delimit frames in a stream)

**--subdirectories | --subdirs | -s**: Create subdirectories when writing files: ssrc/year/month/day/filename

**--ssrc | -S \<ssrc\>**: Select one SSRC (recommended for --stdout)

**--timeout | -t \<seconds\>**: Close file after idle period (default 20 sec)

**--verbose | -v**: Increase verbosity level

**--lengthlimit | --limit | -L \<seconds\>**: maximum file duration in seconds. When new file is created, round down to previous start of interval and pad with silence (for JT decoding)

**--version | -V**: display command version

**--max_length | -x \<seconds\>**: maximum file duration, in seconds. Don't pad the wav file with silence. Exit when all files have reached max duration.

**--ft4 | -4**: same as --jt --lengthlimit 15

**--ft8 | -8**: same as --jt --lengthlimit 7.5

**--wspr | -w**: same as --jt --lengthlimit 120

## Usage

Decode APRS with external program:

```
./pcmrecord -e "direwolf -n 1 -r 24000 -b 16 -" <MCAST_IP>
```
