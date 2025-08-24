# BladeRF

## Description

[BladeRF](https://www.nuand.com/bladerf-1/) is a FPGA based SDR that can tune from 300MHz - 3.8GHz. It has independent RX/TX 12-bit 40MSPS quadrature sampling channels, which is capable of achieving 28Msps per channel.

## SW Installation

## Configuration

See below for an basic example.

```
[global]
hardware = bladerf
status = bladerf.local

[bladerf]
device = bladerf
description = "My BladeRF"
```

Multiple instances of `radiod` can run on the same system, provided each has its own front end (they cannot be shared).

You can have as many as you want, subject to your CPU and USB limits.

The "description" parameter is advertised with mDNS (multicast DNS) service discovery on the LAN and this constrains its content. It should be 63 characters or less and not contain slashes ('/') or control characters (spaces are ok).

### device (mandatory)

In the example above, the `hardware` entry in the `[global]` section specifies the section containing SDR configuration information (in this example the name of the hardware section happens to be the same as the device type, but it is not essential.)

The `device` key is mandatory.

### serial (optional)

If not specified, `radiod` uses the first device discovered. Since this is probably not what you want, you should explicitly specify the serial number if more than one is present.

The `serial` must exactly match the SDR serial number.

### samprate (optional)

Integer, default 12,000,000 (12.0 MHz).

### bandwidth (optional)

Integer, default `samprate` * 0.8.

### gain (optional)

Integer, default AGC on from BladeRF library.

### bias (optional)

Boolean, default false. Enable the bias tee (preamplifier power).
