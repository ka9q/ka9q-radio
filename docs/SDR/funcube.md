# FUNcube Dongle SDR

## Description

The [FUNcube dongle (FCD)](https://www.funcubedongle.com/?page_id=1201) is a 150 kHz to 240 MHz and 420 MHz to 1.9 GHz SDR. The ADC has a sampling rate of 192 ksps.

## SW Installation

The FUNcube dongle enumerates as a USB HID device, and usually no particular SW needs to be installed.

## Configuration

See below for an basic example.

```
[global]
hardware = funcube
status = funcube.local

[funcube]
device = funcube
description = "My FUNcube SDR"
```

You can also reference the [generic config file](/config/radiod@funcube-generic.conf).

Multiple instances of `radiod` can run on the same system, subject to your CPU and USB limits.

The "description" parameter is advertised with mDNS (multicast DNS) service discovery on the LAN and this constrains its content. It should be 63 characters or less and not contain slashes ('/') or control characters (spaces are ok).

### device (mandatory)

In the example above, the `hardware` entry in the `[global]` section specifies the section containing SDR configuration information (in this example the name of the hardware section happens to be the same as the device type, but it is not essential.)

The `device` key is mandatory.

### number (optional)

Integer, default 0. If multiple FCDs are connected, choose which one to open.

### bias (optional)

Boolean, default false. Enable the bias tee (preamplifier power).
