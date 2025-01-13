## Rigexpert Fobos SDR Support

The [Fobos](https://rigexpert.com/software-defined-radio-sdr/fobos-sdr/) is a 100 kHz to 6 GHz SDR with 50 MHz bandwidth and 14-bit signal sampling resolution and the ADC can sample up to 80MS/sec. You can use **ka9q-radio** with the Fobos SDR, but support is optional via dynamic library which must be compiled separately because there are no precompiled drivers available for libfobos, a required driver to use the device.

Fobos is currently only supported on Linux, but MacOS Fobos could be added if `radiod` running on MacOS is fully functional. I've successfully run it on MacOS but had multicast issues



### Installation
1. Install ka9q-radio using the normal documented procedures in [INSTALL.md](INSTALL.md). 
2. Install [libfobos](https://github.com/rigexpert/libfobos)

&nbsp;&nbsp;Slightly changed installation process to update the udev rule to add the `plugdev` group and also `ldconfig` to reload library cache.
```
sudo apt -y install cmake git
git clone https://github.com/rigexpert/libfobos.git
cd libfobos
mkdir build
cd build
cmake ..
make
sudo make install
sudo sed -i 's/TAG+="uaccess"/GROUP="plugdev"/' /etc/udev/rules.d/fobos-sdr.rules
sudo udevadm control --reload-rules
sudo udevadm trigger
sudo ldconfig
```
3. Create the fobos shared object
```
cd ka9q-radio
gcc -c -fPIC -march=native -std=gnu11 -pthread -Wall -funsafe-math-optimizations -fno-math-errno -fcx-limited-range -D_GNU_SOURCE=1 -Wextra -MMD -MP -DNDEBUG=1 -O3 -o fobos-pic.o fobos.c
gcc -shared -o fobos.so fobos-pic.o -L/usr/local/lib -lfobos
sudo rsync -a fobos.so /usr/local/lib
```
4. Edit the config file and run radiod
You may want to first test a basic file like this example:
```
vim config/radiod@fobos-generic.conf
```
You must ensure that you fobos conf files include the lines
```
[fobos]
device = fobos
library = /usr/local/lib/fobos.so
```
exit and
```
radiod config/radiod@fobos-generic.conf 
```