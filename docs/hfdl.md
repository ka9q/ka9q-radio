Using ka9q-radio with HFDL (High Frequency Data Link)  
Phil Karn, KA9Q 28 April 2025
=====================================================

[HFDL](https://en.wikipedia.org/wiki/High_Frequency_Data_Link) is a low speed HF packetized digital communication system used by commercial airliners for long-range communication over oceans and remote areas. It operates in the on-route (civilian) aeronautical HF allocations alongside SSB voice channels used for air traffic control. Each of the 16 ground stations around the world uses a subset of about 105 channels in 12 frequency bands depending on current propagation conditions.

HFDL is part of [ACARS](https://en.wikipedia.org/wiki/ACARS), the Aircraft Communications Addressing and Reporting System, better known for its VHF data links.

What I think most interesting about HFDL for us hams aren't the actual messages but what it can tell us about HF propagation. Hearing a certain ground station or aircraft obviously tells you that the frequency is open between you and that location. The ground stations are in fixed, public locations transmitting periodic beacons ("squitter") messages as well as traffic to specific aircraft. Aircraft transmit their GPS locations, which are often over the oceans or the poles -- places few hams are likely to operate FT8 or WSPR. HFDL overhead messages include aircraft reports of which ground stations have or have not been heard, and how many messages have gotten through at a given data rate. There is much information waiting to be mined from these logs.

*ka9q-radio* can feed the *dumphfdl* decoder by Tomasz Lemiech szpajder@gmail.com and the results automatically forwarded to the crowd sourced public database at [Airframes](https://app.airframes.io/). As with FT4, FT8 and WSPR, with a wideband front end like the RX888 MKii covering all of HF every HFDL channel can be monitored and decoded simultaneously.  HFDL decoding can usually share the same front ends and computers already skimming and recording FT4, FT8, WSPR, WWV, etc. The CPU load is minimal so there is no need to shut down decoders on inactive channels.

Setting this up is a bit involved because *dumphfdl*, like *ka9q-radio*, is not yet available in a Debian package that can be easily installed with **apt-get**; it must be built from the its [Github repository](https://github.com/szpajder/dumphfdl). *dumphfdl* in turn requires [libacars](https://github.com/szpajder/libacars) by the same author. Both packages have dependencies that can be satisfied with **apt-get**.

## Installing *dumphfdl* and *libacars*

Start by cloning both respositories:
```
git clone https://github.com/szpajder/libacars
git clone https://github.com/szpajder/dumphfdl
```

Now install their various dependencies. (This list may not be complete. If you know of more, please let me know).
```
sudo apt install cmake zlib1g-dev libxml2-dev libjansson-dev libliquid-dev libsoapysdr-dev libfftw3-dev libzmq3-dev libsqlite3-dev libglib2.0-dev libconfig++-dev

```

Build and install *libacars*:
```
cd libacars
mkdir build
cd build
cmake ..
make -j
sudo make install
```

Starting again from your top-level directory, build and install *dumphfdl*:
```
cd dumphfdl
mkdir build
cd build
cmake ..
make -j
sudo make install
```
The -j option to *make* speeds compilation considerably on multicore systems. If this causes trouble, remove it or specify a limit, e.g, **make -j 4**.

## Configuring *ka9q-radio* to feed *dumphfdl*
The 
The *hfdl* systemd service file launches the *pcmrecord* program, which in turn runs a shell helper script that starts one instance of *dumphfdl* for each band and feeds it the signals for that band.

Start by adding the band/channel list to your *radiod* config file. You can append this fragment from my own HF configuration:

```
ka9q-radio/config/radiod@ka9q-hf.conf.d/50-hfdl.conf
```

Be sure "disable = no" is set at the top of *each* section. Restart *radiod*.

Now configure and start the decoders:

```
cd ka9q-radio/config # go into the config files distributed with ka9q-radio
cp hfdl.conf /etc/radio # edit if necessary
sudo systemctl enable hfdl
sudo systemctl start hfdl
```

The service file in /etc/systemd/system/hfdl.service launches *pcmrecord*, which in turn runs a helper script that starts an instance of *dumphfdl* for each HF band. These instances read from the channels defined in your *radiod* config file that you edited earlier.
For convenience, running **make install** in *ka9q-radio* installs a seed version of the HFDL system table in /var/lib/hfdl/systable.conf. This will be updated in place by *dumphfdl*, so the *ka9q-radio* makefile will not overwrite it if it already exists.

At this point, the decoders should be running and reporting data. You can watch their progress with
```
tail -f /var/log/hfdl.log
```

You should go to the [Airframes](https://app.airframes.io/) site, create an account, and claim ownership of your feeds. Your station identification on their list is taken from the "description" parameter in the hardware section of your *radiod* config file, so don't use something generic (e,g. "rx888-wsprdaemon"). Since most of us have more than one antenna, I use descriptions like "w6lvp loop @ KA9Q" or "g5rv @ KA9Q.


