# Configuring and Running ka9q-radio - Part 2

Phil Karn, KA9Q

## Hardware Configuration

This document describes the hardware definition section in a `radiod` config file. The section name must match the **hardware** entry in the `[global]` section, e.g.

```
[global]
hardware = airspy
...

[airspy]
device = airspy
description = "airspy on 2m antenna"
```

In this example the name of the hardware definition section matches the device type, but this is not required.

The **description** is advertised with mDNS (multicast DNS) service discovery on the LAN and this constrains its content. It should be 63 characters or less and not contain slashes ('/') or control characters (spaces are ok).

## Supported Hardware

Multiple SDR front ends are currently supported in `ka9q-radio`:

| Frond end                 | Description                               | Comment |
|---------------------------|-------------------------------------------|---------|
| [airspy](SDR/airspy.md)   | Airspy R2, Airspy Mini                    | OS driver
| [airspyhf](SDR/airspy.md) | Airspy HF+                                | OS driver
| [bladerf](SDR/bladerf.md) | BladeRF                                   | OS driver
| [fobos](SDR/fobos.md)     | Fobos SDR                                 | BYO driver
| [funcube](SDR/funcube.md) | AMSAT-UK FUNcube (Pro+)                   |
| [hackrf](SDR/hackrf.md)   | HackRF One                                | OS driver
| [hydra](SDR/hydra.md)     | HydraSDR                                  | BYO driver
| [rtlsdr](SDR/rtlsdr.md)   | Generic RTL-SDR dongle                    | OS driver
| [rx888](SDR/rx888.md)     | RX888 MKII (direct conversion only)       |
| [sdrplay](SDR/sdrplay.md) | SDRplay                                   | BYO driver
| [sig_gen](sig_gen.md)     | synthetic front end with signal generator |

## Configuration Options

The configuration of each device type is necessarily hardware-dependent, so separate documents describe the options unique to each one. Only the parameters common to all of them are described here. In most cases, the default hardware-specific options need not be changed.

### device (no default, required front end name)

Select the front end hardware type. If there is only one such device on a system, it will automatically be selected. If there's more than one, it can usually be selected by serial number.

The FUNcube does not have serial numbers so this is not possible.

Support for multiple RX888s (which has serial numbers) is not yet supported. I don't recommend more than one per system because of the heavy load they place on the USB controller. Each RX888 running at full sample rate generates a little over 2 Gbps of data.

### description (no default, optional but recommended)

Gives free-format text that will be advertised through the `radiod` program to the control/status stream and the `control` program that listens to it. It will also be advertised in DNS SRV (service discovery) records by the Linux mDNS daemon `avahi`, so keep it short but descriptive.

[Part 3](ka9q-radio-3.md) describes the configuration of channel groups.
