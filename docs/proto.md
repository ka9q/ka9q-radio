# Status / Control TLV Specification (Draft)

This document describes the TLV (Type–Length–Value) elements used by the
status and control protocol.

This is a **working document**. It is expected to be edited, corrected,
and extended alongside the source code.

## General Rules

- TLV type numbers are wire-stable.
- Do **not** remove or reorder existing TLV types in code.
- New TLV types must be appended. Retired types must be marked as unused, not deleted.
- Type 0 (`EOL`) is a special end-of-list marker and has no length field.

Columns used below:

- **Type** – numeric TLV ID
- **Name** – symbolic enum name
- **Object type** – integer / float / string / vector / socket / etc.
- **Access** – R/O, R/W, command
- **Description** – human-readable meaning

---

## Core / Control

| Type | Name | Object type | Access | Scope | Description |
|-----:|------|-------------|--------|-------|-------------|
| 0 | EOL | | | End of TLV list |
| 1 | COMMAND_TAG | integer | R/W | channel | arbitrary 32-bit integer echoed from command packets
| 2 | CMD_CNT | integer | R/O | global | Count of commands received since startup |
| 3 | GPS_TIME | integer | R/O | global | count of nanoseconds since GPS epoch - derived from Linux system time |
| 4 | DESCRIPTION | string | R/O | global | free-form description of antenna and receiver configured into radiod |
| 5 | STATUS_DEST_SOCKET | socket | R/O | global | Multicast group address for commands and responses |
| 6 | SETOPTS | | | |
| 7 | CLEAROPTS | | | |
| 8 | RTP_TIMESNAP | integer | R/O | channel | Next RTP timestamp to be used on logical channel|

---

## Spectrum / FFT Analyzer

| Type | Name | Object type | Access | Description |
|-----:|------|-------------|--------|-------------|
| 9 | BIN_BYTE_DATA | | | |
| 10 | INPUT_SAMPRATE | | | |
| 11 | SPECTRUM_BASE | | | |
| 12 | SPECTRUM_AVG | | | |
| 13 | INPUT_SAMPLES | | | |
| 14 | WINDOW_TYPE | | | |
| 15 | NOISE_BW | | | |
| 72 | FFT_SIZE | | | |
| 73 | FFT_BIN | | | |
| 74 | FFT_WINDOW | | | |
| 75 | FFT_OVERLAP | | | |
| 76 | FFT_SHIFT | | | |
| 77 | FFT_RATE | | | |
| 78 | FFT_INPUT_RATE | | | |
| 79 | FFT_SAMPLES | | | |
| 80 | FFT_CNT | | | |
| 81 | FFT_MAX | | | |
| 82 | FFT_MIN | | | |
| 83 | FFT_MEAN | | | |
| 84 | FFT_VAR | | | |
| 85 | FFT_SDEV | | | |
| 86 | FFT_MEDIAN | | | |
| 87 | FFT_95PCT | | | |
| 88 | FFT_99PCT | | | |
| 89 | FFT_999PCT | | | |
| 90 | FFT_9999PCT | | | |
| 91 | FFT_DBFS | | | |


---

## Frontend / Hardware

| Type | Name | Object type | Access | Description |
|-----:|------|-------------|--------|-------------|
| 94 | A_D_RANGE | | | |
| 95 | A_D_PEAK | | | |
| 96 | FRONTEND_FC | | | |
| 97 | FRONTEND_LO | | | |
| 98 | FRONTEND_RATE | | | |
| 99 | FRONTEND_LAG | | | |
| 100 | FRONTEND_ERRORS | | | |
| 101 | FRONTEND_MDNS | | | |
| 102 | FRONTEND_DESCRIPTION | | | |
| 103 | FRONTEND_VERSION | | | |
| 104 | FRONTEND_REFCLK | | | |
| 105 | FRONTEND_GPSD | | | |
| 106 | FRONTEND_INPUT | | | |
| 107 | FRONTEND_PATH | | | |
| 108 | FRONTEND_SAMPLES | | | |
| 109 | FRONTEND_PLL | | | |
| 110 | FRONTEND_GAIN | | | |
| 111 | FRONTEND_AGC | | | |
| 112 | FRONTEND_ATTEN | | | |
| 113 | FRONTEND_PREAMP | | | |
| 114 | FRONTEND_ANTENNA | | | |
| 115 | FRONTEND_PORT | | | |

---

## Signal Metrics

| Type | Name | Object type | Access | Description |
|-----:|------|-------------|--------|-------------|
| 41 | IF_POWER | | | |
| 42 | BASEBAND_POWER | | | |
| 43 | NOISE_DENSITY | | | |

---

## Demodulation Configuration

| Type | Name | Object type | Access | Description |
|-----:|------|-------------|--------|-------------|
| 44 | DEMOD_TYPE | | | |
| 45 | OUTPUT_CHANNELS | | | |
| 46 | INDEPENDENT_SIDEBAND | | | |
| 47 | PLL_ENABLE | | | |
| 48 | PLL_LOCK | | | |
| 49 | PLL_SQUARE | | | |
| 50 | PLL_PHASE | | | |
| 51 | PLL_BW | | | |
| 52 | ENVELOPE | | | |
| 53 | SNR_SQUELCH | | | |

---

## Demodulation Status

| Type | Name | Object type | Access | Description |
|-----:|------|-------------|--------|-------------|
| 54 | DEMOD_SAMPRATE | | | |
| 55 | DEMOD_SAMPLES | | | |
| 56 | DEMOD_CNT | | | |
| 57 | DEMOD_DELAY | | | |
| 58 | DEMOD_RSSI | | | |
| 59 | DEMOD_SNR | | | |
| 60 | DEMOD_NOISE_DENSITY | | | |
| 61 | DEMOD_PEAK | | | |
| 62 | DEMOD_OVERRANGES | | | |
| 63 | DEMOD_GROUP | | | |

---

## Audio / Opus

| Type | Name | Object type | Access | Description |
|-----:|------|-------------|--------|-------------|
| 64 | OPUS_ENABLE | | | |
| 65 | OPUS_BITRATE | integer | r/w | set nominal Opus encoder bitrate. 0= auto |
| 66 | OPUS_COMPLEXITY | | | |
| 67 | OPUS_DTX | | | |
| 68 | OPUS_APPLICATION | | | |
| 69 | OPUS_BANDWIDTH | | | |
| 70 | OPUS_FEC | | | |
| 71 | SPECTRUM_STEP | | | |

---

## Notes

- Object types and access permissions are intentionally incomplete.
- This document should be kept in sync with `status.h` and `status.c`.
- When in doubt, the code is authoritative.
