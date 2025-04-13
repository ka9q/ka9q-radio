# RigExpert Fobos SDR

Phil Karn, KA9Q

## Description

The [Fobos SDR](https://rigexpert.com/software-defined-radio-sdr/fobos-sdr/) is a 100 kHz to 6 GHz SDR with 50 MHz bandwidth and 14-bit signal sampling resolution. The ADC can sample up to 80 Msps.

## SW Installation

You can use **ka9q-radio** with the Fobos SDR, but support is currently optional because `libfobos`, required to use the device, is not yet available as a Debian Linux package. It must be separately downloaded and installed.

Fobos is currently only supported on Linux, but MacOS Fobos could be added if `radiod` running on MacOS is fully functional. I've successfully run it on MacOS but had multicast issues.

Install [libfobos](https://github.com/rigexpert/libfobos) using the documented procedure.

The only change to these steps are the addition of running `ldconfig` to refresh the library cache.

```
sudo apt -y install cmake git
git clone https://github.com/rigexpert/libfobos.git
cd libfobos
mkdir build
cd build
cmake ..
make
sudo make install
sudo udevadm control --reload-rules
sudo udevadm trigger
sudo ldconfig
```

Build and install **ka9q-radio** using the normal documented procedures in [INSTALL.md](/docs/INSTALL.md).

If `libfobos` was properly installed, **ka9q-radio** will automatically build with Fobos support. If you've already installed **ka9q-radio**, just re-run make and the *fobos.so* driver should build automatically.

Run these commands to modify the udev rule that was included with libfobos to include the `plugdev` group.

```
sudo sed -i 's/TAG+="uaccess"/GROUP="plugdev"/' /etc/udev/rules.d/fobos-sdr.rules
sudo udevadm control --reload-rules
sudo udevadm trigger
```

## Configuration

See below for an basic example.

```
[global]
hardware = fobos
status = fobos.local

[fobos]
device = fobos
description = "My Fobos SDR"
```

You can also reference the [generic config file](/config/radiod@fobos-generic.conf).

Multiple instances of `radiod` can run on the same system, provided each has its own front end (they cannot be shared).

You can have as many as you want, subject to your CPU and USB limits.

### device (mandatory)

In the example above, the `hardware` entry in the `[global]` section specifies the section containing Airspy configuration information. (In this example the name of the hardware section happens to be the same as the device type, but it is not essential.)

The `device` key is mandatory.

### serial (optional)

If not specified, `radiod` uses the first device discovered. Since this is probably not what you want, you should explicitly specify the serial number if more than one is present.

The `serial` must exactly match the SDR serial number, in hex (the leading 0x is optional).

### samprate (optional)

Double, default is the highest speed advertised by the device, usually 80 MHz.

### clk_source (optional)

Integer, default 0 (internal 1). If 1 use external CLKIN (untested).

### direct_sampling (optional)

Boolean, default false (RF Input). If true, direct sampling on HF1 and/or HF2 inputs.

### hf_input (optional)

Integer, default 0. HF input selection in direct sampling mode: 0 -> I/Q; 1 -> HF1 only; 2 -> HF2 only.

### lna_gain (optional)

Integer, default 0. If 0 or 1 -> 0 dB; 2 -> +16 dB; 3 -> +33 dB (disregarded in direct sample mode).

### vga_gain (optional)

Integer, default 0. 0 to +62 dB in 2 dB steps (disregarded in direct sample mode).
