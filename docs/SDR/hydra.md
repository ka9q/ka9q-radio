# HydraSDR

## Description

[HydraSDR RFone](https://hydrasdr.com/products/) is a SDR capable of sampling 10MHz of spectrum anywhere between 24MHz to 1800MHz and even beyond with extensions. The sampling range is approx. 20 Msps.

## SW Installation

## Configuration

See below for an basic example.

```
[global]
hardware = hydra
status = hydra.local

[funcube]
device = rtlsdr
description = "My HydraSDR"
```

You can also reference the [generic config file](/config/radiod@hydra-generic.conf).

Multiple instances of `radiod` can run on the same system, subject to your CPU and USB limits.

The "description" parameter is advertised with mDNS (multicast DNS) service discovery on the LAN and this constrains its content. It should be 63 characters or less and not contain slashes ('/') or control characters (spaces are ok).

### device (mandatory)

In the example above, the `hardware` entry in the `[global]` section specifies the section containing SDR configuration information (in this example the name of the hardware section happens to be the same as the device type, but it is not essential.)

The `device` key is mandatory.

