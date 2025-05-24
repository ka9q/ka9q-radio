# monitor

`monitor` is the **Multicast Audio Monitor** utility integrated in **ka9q-radio** SW.

```
monitor [OPTIONS]... <MCAST_IP>
```

## Description

This will start a CLI where you can see details about every monitored session (freq, mode, SNR, ...), and hear the audio output.

By default the audio for all active sessions will be played on your default audio device.

Depending on the configuration it is OK for `monitor` to start without audio and sessions.

A session does not appear in the list until it's active (audio is decoded).

### CLI Options

**-I, --input**

> multicast IP to monitor. Can be specified multiple.times to monitor multiple sources.

**-L, --list-audio**

> list audio devices and exit.

**-R, --device**

> output to audio device, default use PortAudio.

**-S, --autosort**

> enable auto-sorting sessions for TUI, default false.

**-c, --channels**

> audio channels, default 2.

**-f, --config**

> config file path.

**-g, --gain**

> audio gain, default 0 (no gain).

**-n, --notch**

> enable notch filter, default false.

**-o, --source**

> select only one source from a multicast group.

**-P, --pipe**

> output to named pipe.

**-p, --playout**

> playout delay, default 100.

**-q, --quiet**

> do not display TUI, default false.

**-r, --samprate**

> audio sample rate, default 48000.

**-s, --voting**

> enable voting (automatic selection of repeater), default false.

**-u, --update**

> update interval, default 0.

**-v, --verbose**

> show more messages, default false.

**-V, --version**

> show version and exit.

### Runtime Commands

| keybind                 | description |
|-------------------------|-------------|
| Q                       | quit monitor entirely |
| ↑ ↓ (arrow up/down)     | select prev/next session |
| ⤒ ⤓ (home/end)          | select first/last session |
| ⇞ ⇟ (page up/down)      | select prev/next session page |
| d                       | delete session |
| r                       | reset playout buffer |
| R                       | reset all playout buffers |
| m                       | mute current session |
| M                       | mute all sessions |
| u                       | unmute current session |
| U                       | unmute all sessions |
| f                       | turn off PL notch |
| F                       | turn off PL notch, all sessions |
| n                       | turn on PL notch |
| N                       | turn on PL notch, all sessions |
| A                       | toggle, start all future sessions muted |
| C                       | toggle, constant playout delay |
| s                       | sort sessions by most recently active |
| S                       | autosort sessions by most recently active |
| t                       | sort sessions by most active |
| - +                     | volume -1/+1 dB |
| ← → (arrow left/right)  | stereo position left/right |
| SHIFT + ←               | playout buffer -1 ms |
| SHIFT + →               | playout buffer +1 ms |
| v                       | toggle, verbose display |
| q                       | toggle, quiet mode |

### IDs

If you want to show IDs for your `monitor` sessions you have to create (or edit) the file */usr/local/share/ka9q-radio/id.txt*.

The format of this file is:

```
freq1 [tone] ID of freq1
freq2 [tone] ID of freq2
```

Where **freq** is the frequency in Hz, **tone** is the optional CTCSS/PL tone in Hz, and the rest of the line is the ID.
