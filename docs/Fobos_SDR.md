## Rigexpert Fobos SDR Support

The [Fobos](https://rigexpert.com/software-defined-radio-sdr/fobos-sdr/) is a 100 kHz to 6 GHz SDR with 50 MHz bandwidth and 14-bit signal sampling resolution. The ADC can sample up to 80MS/sec. You can use **ka9q-radio** with the Fobos SDR, but support is optional because there are no precompiled drivers available for libfobos (the  driver required to use the device).

Fobos is currently only supported on Linux, but MacOS Fobos could be added if `radiod` running on MacOS is fully functional. I've successfully run it on MacOS but had multicast issues



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
2. Install ka9q-radio using the normal documented procedures in [INSTALL.md](INSTALL.md) with a few slight changes (appending FOBOS=1) to the make commands and modifying the udev rule that was included with libfobos to include the `plugdev` group.

```
make FOBOS=1
sudo make install FOBOS=1
sudo sed -i 's/TAG+="uaccess"/GROUP="plugdev"/' /etc/udev/rules.d/fobos-sdr.rules
sudo udevadm control --reload-rules
sudo udevadm trigger
```

3. Edit the config file and run radiod
You may want first to test a basic file like this example:
```
vim config/radiod@fobos-generic.conf
```
You must ensure that your fobos conf files include the lines:
```
[fobos]
device = fobos
library = /usr/local/lib/fobos.so
```
exit and
```
radiod config/radiod@fobos-generic.conf 
```
