# RTL-SDR

Phil Karn, KA9Q

## Description

[RTL-SDR](https://www.rtl-sdr.com/about-rtl-sdr/) is any SDR dongle based on DVB_T tuner ICs (Elonics, Rafael Micro, Fitipower, ...), usually with a RTL2832U demodulator / USB interface. The frequency range is approx. 24 MHz - 1.7 GHz and the sampling rate is approx. 2.5 Msps.

## SW Installation

RTL-SDR is supported by default on most linux distributions by the `librtlsdr-dev` package.

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

The "description" parameter is advertised with mDNS (multicast DNS) service discovery on the LAN and this constrains its content. It should be 63 characters or less and not contain slashes ('/') or control characters (spaces are ok).

### device (mandatory)

In the example above, the `hardware` entry in the `[global]` section specifies the section containing SDR configuration information (in this example the name of the hardware section happens to be the same as the device type, but it is not essential.)

The `device` key is mandatory.

### serial (optional)

If not specified, `radiod` uses the first device discovered. Since this is probably not what you want, you should explicitly specify the serial number if more than one is present.

The `serial` must exactly match the SDR serial number.

You can find the serial number with the `rtl_eeprom` utility.

```
>$ rtl_eeprom
Found 1 device(s):
  0:  Generic RTL2832U OEM

Using device 0: Generic RTL2832U OEM
Detached kernel driver
Found Rafael Micro R820T tuner

Current configuration:
__________________________________________
Vendor ID:              0x0bda
Product ID:             0x2838
Manufacturer:           Nooelec
Product:                NESDR SMArt v5
Serial number:          00000001
Serial number enabled:  yes
IR endpoint enabled:    yes
Remote wakeup enabled:  no
__________________________________________
Reattached kernel driver
```

### samprate (optional)

Integer, default 1,800,000 (1.8 MHz).

### direct_sampling (optional)

Integer, default 0 (direct sampling disabled). If 1 use I input, if 2 use Q input.

### agc (optional)

Boolean, default false. Enable the hardware AGC.

### bias (optional)

Boolean, default false. Enable the bias tee (preamplifier power).

### gain (optional)

Float, default 0.0.
