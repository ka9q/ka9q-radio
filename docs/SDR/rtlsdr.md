# RTL-SDR

Phil Karn, KA9Q

## Description

## SW Installation

[RTL-SDR](https://www.rtl-sdr.com/about-rtl-sdr/) is any SDR dongle based on DVB_T tuner ICs (Elonics, Rafael Micro, Fitipower, ...), usually with a RTL2832U demodulator / USB interface. The frequency range is approx. 24 MHz - 1.7 GHz and the sampling rate is approx. 2.5 Msps.

## Configuration

See below for an basic example.

```
[global]
hardware = rtlsdr
status = rtlsdr.local

[funcube]
device = rtlsdr
description = "My RTL-SDR"
```

You can also reference the [generic config file](/config/radiod@rtlsdr-generic.conf).

Multiple instances of `radiod` can run on the same system, provided each has its own front end (they cannot be shared).

You can have as many as you want, subject to your CPU and USB limits.

### device (mandatory)

In the example above, the `hardware` entry in the `[global]` section specifies the section containing SDR configuration information (in this example the name of the hardware section happens to be the same as the device type, but it is not essential.)

The `device` key is mandatory.

### serial (optional)

If not specified, `radiod` uses the first device discovered. Since this is probably not what you want, you should explicitly specify the serial number if more than one is present.

The `serial` must exactly match the SDR serial number.

### samprate (optional)

Integer, default is 1.8 MHz (1800000).

### agc (optional)

Boolean, default false. Enable the hardware AGC.

### bias (optional)

Boolean, default false. Enable the bias tee (preamplifier power).

### gain (optional)

Float, default 0.0.
