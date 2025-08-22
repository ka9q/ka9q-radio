#!/bin/bash
# Install script for the RPi, contributed by Martin Levy, W6LHI/G8LHI

# Just so it's documented somewhere; here's all the dependency
# requirements for building on a R.Pi. (In my case, a Pi5 running
# 64bit Lite OS). Install (i.e. /usr/local/bin etc) is not included
# here. It should go into the Makefile first.

# Start from scratch with a minimal R.Pi
# I loaded up Raspberry Pi OS Lite (64bit) - i.e. no graphical display

# install basic tools to fetch from github, compile, make, etc
# some of these are already on the base system - but here to be complete
sudo apt install -y git g++ gcc make

# many dependencies are needed - install all of these
sudo apt install -y libavahi-glib-dev libavahi-client-dev libavahi-core-dev
sudo apt install -y libfftw3-dev
sudo apt install -y libbsd-dev
sudo apt install -y libopus-dev
sudo apt install -y libncurses5-dev
sudo apt install -y libusb-1.0-0-dev

# these are needed for the supported physical devices
sudo apt install -y librtlsdr-dev libairspy-dev libairspyhf-dev

# you have to build iniparser by hand - easy easy
mkdir -p ~/src/github/ndevilla/
cd ~/src/github/ndevilla/
git clone https://github.com/ndevilla/iniparser.git
cd iniparser/
make -j4

# install iniparser
sudo cp libiniparser.* /usr/local/lib/
sudo mkdir /usr/local/include/iniparser
sudo cp src/dictionary.h /usr/local/include/
sudo cp src/iniparser.h /usr/local/include/iniparser/

# finally grab ka9q-radio
mkdir -p ~/src/github/ka9q
cd ~/src/github/ka9q
git clone https://github.com/ka9q/ka9q-radio.git
 cd ka9q-radio/

# finally - make ka9q-radio
make -j4 INCLUDES=-I/usr/local/include -f Makefile.linux

# takes around 7.5 seconds to compile on R.Pi5 with NVMe SSD drive
  
