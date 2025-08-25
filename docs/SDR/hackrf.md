# HackRF

## Description

[HackRF One](https://greatscottgadgets.com/hackrf/one/) is a SDR capable of transmission or reception of radio signals from 1 MHz to 6 GHz. It has a half-duplex transceiver and a sample rate of approx. 20 Msps.

## SW Installation

HackRF One is supported by default on most linux distributions by the `libhackrf-dev` package.

## Configuration

See below for an basic example.

```
[global]
hardware = hackrf
status = hackrf.local

[funcube]
device = hackrf
description = "My HackRF"
```

You can also reference the [generic config file](/config/radiod@hackrf-generic.conf).

Multiple instances of `radiod` can run on the same system, subject to your CPU and USB limits.

The "description" parameter is advertised with mDNS (multicast DNS) service discovery on the LAN and this constrains its content. It should be 63 characters or less and not contain slashes ('/') or control characters (spaces are ok).

### device (mandatory)

In the example above, the `hardware` entry in the `[global]` section specifies the section containing SDR configuration information (in this example the name of the hardware section happens to be the same as the device type, but it is not essential.)

The `device` key is mandatory.

### serial (optional)

