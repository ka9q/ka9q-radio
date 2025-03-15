## Rigexpert Fobos SDR Support

Updated 2 Feb 2025 by KA9Q

The [Fobos](https://rigexpert.com/software-defined-radio-sdr/fobos-sdr/) is a 100 kHz to 6 GHz SDR with 50 MHz bandwidth and 14-bit signal sampling resolution. The ADC can sample up to 80MS/sec. You can use **ka9q-radio** with the Fobos SDR, but support is
currently optional because libfobos, required to use the device, is not yet available as a Debian Linux package. It must be
separately downloaded and installed.

Fobos is currently only supported on Linux, but MacOS Fobos could be added if `radiod` running on MacOS is fully functional. I've successfully run it on MacOS but had multicast issues.


### Installation
1. Install [libfobos](https://github.com/rigexpert/libfobos) using the documented procedures

&nbsp;&nbsp;The only change to these steps are the addition of running `ldconfig` to refresh the library cache.
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
2. Build and install ka9q-radio using the normal documented procedures in [INSTALL.md](INSTALL.md). If libfobos was
properly installed, ka9q-radio will automatically build with Fobos support. If you've already installed ka9q-radio, just re-run make and the fobos.so driver should build automatically.

3. Run these commands to modify the udev rule that was included with libfobos to include the `plugdev` group.

```
sudo sed -i 's/TAG+="uaccess"/GROUP="plugdev"/' /etc/udev/rules.d/fobos-sdr.rules
sudo udevadm control --reload-rules
sudo udevadm trigger
```

4. Edit the config file and run radiod
You may want first to test a basic file like this example:
```
vim config/radiod@fobos-generic.conf
```
Note these lines in the [fobos] section. If the "device = fobos" line is missing, the section name must be [fobos].
The default location for the shared library is /usr/local/lib/ka9q-radio/fobos.so; this may be overridden with the "library =" line.
```
[fobos]
device = fobos
library = /usr/local/lib/ka9q-radio/fobos.so
```
exit and
```
radiod config/radiod@fobos-generic.conf
```
