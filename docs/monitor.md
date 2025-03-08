# monitor

`monitor` is the **Multicast Audio Monitor** utility integrated in **ka9q-radio** SW.

## Usage

Start monitor by running

```
monitor MCAST_IP
```

This will start a CLI where you can see details about every monitored session (freq, mode, SNR, ...), and hear the audio output.

By default the audio for all active sessions will be played on your default audio device.

Depending on the configuration it is OK for `monitor` to start without audio and sessions.

A session does not appear in the list until it's active (audio is decoded).

## Commands

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

## IDs

If you want to show IDs for your `monitor` sessions you have to create the file */usr/local/share/ka9q-radio/id.txt*.

The format of this file is:

```
freq1 [tone] ID of freq1
freq2 [tone] ID of freq2
```

Where **freq** is the frequency in Hz, **tone** is the optional CTCSS/PL tone in Hz, and the rest of the line is the ID.
