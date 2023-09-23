Configuring and Running *ka9q-radio - Part 2
============================================

v1.0 (in progress), September 2023  
Phil Karn, KA9Q
---------------

Hardware Configuration
----------------------

This document describes the hardware definition section in a *radiod*
config file.  The section name must match the **hardware** entry in
the [global] section, e.g.,

[global]  
hardware = airspy  
...

[airspy]  
device = airspy  
description = "airspy on 2m antenna"


In this example the name of the hardware definition section matches
the device type, but this is not required.

Supported Hardware
------------------

Six SDR front ends are currently supported in *ka9q-radio*:

[airspy](airspy.md) - Airspy R2, Airspy Mini]  
[airspyhf](airspy.md) - Airspy HF+  
[funcube](funcube.md) - AMSAT UK Funcube Pro+ dongle  
[rx888](rx888.md) - RX888 Mkii (direct conversion only)  
[rtlsdr](rtlsdr.md) - Generic RTL-SDR dongle (VHF/UHF only)  
[sig_gen][sig_gen.md] - synthetic front end with signal generator (to be documented)

The configuration of each device type is necessarily
hardware-dependent, so separate documents describe the options unique
to each one. Only the parameters common to all of them are described
here. In most cases, the default hardware-specific options need not be changed.

### device = {airspy|airspyhf|funcube|rx888|rtlsdr|sig_gen} (no default, required)

Select the front end hardware type. If there is only one such device
on a system, it will automatically be selected. If there's more than one,
it can usually be selected by serial number.

The funcube does not have serial
numbers so this is not possible.

Support for multiple rx888s (which has serial numbers) is not yet supported.
I don't recommend more than one per system because of the heavy load they place on the USB controller.
Each rx888 running at full sample rate generates a little over 2 Gb/s of data.x


### description = (no default, optional but recommended)

Gives free-format text that
will be advertised through the *radiod* program to the
control/status stream and the *control* program that
listens to it. It will also be advertised in DNS SRV (service
discovery) records by the Linux mDNS daemon *avahi*, so keep
it short but descriptive.

[Part 3](ka9q-radio-3.md) describes the configuration of channel groups.
