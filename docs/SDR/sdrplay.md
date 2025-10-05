# SDRplay

## Description

[SDRplay](https://www.sdrplay.com/products/) is a family of SDRs.

## SW Installation

## Configuration

See below for an basic example.

```
[global]
hardware = sdrplay
status = sdrplay.local

[funcube]
device = sdrplay
description = "My RSP1B"
```

You can also reference the [generic config file](/config/radiod@sdrplay-generic.conf).

Multiple instances of `radiod` can run on the same system, subject to your CPU and USB limits.

The "description" parameter is advertised with mDNS (multicast DNS) service discovery on the LAN and this constrains its content. It should be 63 characters or less and not contain slashes ('/') or control characters (spaces are ok).

### device (mandatory)

In the example above, the `hardware` entry in the `[global]` section specifies the section containing SDR configuration information (in this example the name of the hardware section happens to be the same as the device type, but it is not essential.)

The `device` key is mandatory.

### serial (optional)

