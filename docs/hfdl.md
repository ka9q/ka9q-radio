Using ka9q-radio with HFDL (High Frequency Data Link)  
Phil Karn, KA9Q 28 April 2025
=====================================================

[HFDL](https://en.wikipedia.org/wiki/High_Frequency_Data_Link) is a low speed HF packetized digital communication system used by commercial airliners for long-range communication over oceans and remote areas. It operates in the on-route (civilian) aeronautical HF allocations alongside SSB voice channels used for air traffic control. Each of the 15 ground stations around the world uses a subset of about 105 channels in 12 frequency bands depending on current propagation conditions.

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
sudo apt install cmake zlib1g-dev libxml2-dev libjansson-dev libliquid-dev libsoapysdr-dev libfftw3-dev libzmq3-dev libsqlite3-dev

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
Now the fun part. There are two ways to run *dumphfdl* with *ka9q-radio*. One runs a separate *ka9q-radio* channel and *dumphfdl* instance for every channel. The second runs one *ka9q-radio* channel and *dumphfdl* instance for every aeronautical *band*, with *dumphfdl* extracting the individual channels on each band. There are advantages to each method.

The per-channel approach is considerably simpler to configure, as *systemd* need manage only one *hfdl* service. The individual channel frequencies are specifed only in the *radiod* configuration file, and all output is sent to the same IP multicast group, hfdl.local (each with its own RTP SSRC, of course). The **pcmrecord** program (part of *ka9q-radio*) with the **--exec** option automatically launches a separate instance of *dumphfdl* for each channel passing the appropriate parameters to each channel. All log into the same file, */var/log/hfdl.log*. The drawback to this method is that there are over a hundred HFDL channels so there will be over a hundred instances of *dumphfdl*, each with its own TCP/IP connection to feed.airframes.io.

The per-band approach uses only one *ka9q-radio* channel and one *dumphfdl* instance per band, but the *ka9q-radio* channels are relatively wide and are different for each band, requiring a separate IP multicast group for each band (12 total). *systemd* must manage 12 separate instances of the *hfdl* service, with only 12 connections to feed.airframes.io. This may use somewhat less total CPU time than the first method, though the jury is still out on this question. Each sample rate also requires a separate run of **fftwf-wisdom** to optimize the inverse FFT inside *radiod* for that rate, though this doesn't seem to be a major problem.

### Per-channel method

Add the channel list to your *radiod*'s config file. You can use this fragment from my own HF configuration:

ka9q-radio/config/radiod@ka9q-hf.conf.d/55-hfdl-sep.conf

Note that I use the optional subdirectory feature for my own configuration. Eventually this will make it easier to share configuration file fragments between multiple users and configurations.

Be sure to set "disable = no" at the beginning of the section.

Restart *radiod*, configure and start the decoders:

```
sudo systemctl try-restart 'radiod@*' # or specify the actual radiod instance
cd ka9q-radio/config # go into the config files distributed with ka9q-radio
cp hfdl.conf /etc/radio # edit if necessary
systemctl enable hfdl
systemctl start hfdl
```

For convenience, running **make install** in *ka9q-radio* installs a seed version of the HFDL system table in /var/lib/hfdl/systable.conf. This will be updated in place by *dumphfdl*, so the *ka9q-radio* makefile will not overwrite it if it already exists.

At this point, the decoders should be running and reporting data. You can watch their progress with
```
tail -f /var/log/hfdl.log
```

### Per-band method

Insert this fragment from my HF configuration into your *radiod* config file:

ka9q-radio/config/radiod@ka9q-hf.conf.d/50-hfdl.conf

Be sure "disable = no" is set at the top of *each* section.

Restart *radiod*, configure and start the decoders, one instance for each band:

```
sudo systemctl try-restart 'radiod@*' # or specify the actual radiod instance
cd ka9q-radio/config # go into the config files distributed with ka9q-radio
cp hfdl-*.conf /etc/radio # e.g., hfdl-8.conf, hfdl-21.conf. These are distinguished from the per-channel configuration with a band suffix.
systemctl enable hfdl@2 hfdl@3 hfdl@4 hfdl@5 hfdl@6 hfdl@8 hfdl@10 hfdl@11 hfdl@13 hfdl@15 hfdl@17 hfdl@21 # one instance for each band
systemctl start hfdl@2 hfdl@3 hfdl@4 hfdl@5 hfdl@6 hfdl@8 hfdl@10 hfdl@11 hfdl@13 hfdl@15 hfdl@17 hfdl@21 
```
The decoders should be up and running. The individual logs will be in the subdirectory */var/log/hfdl* and you can watch them all with,

```
tail -f /var/log/hfdl/*.log
```

You should go to the [Airframes](https://app.airframes.io/) site, create an account, and claim ownership of your feeds. Your station identification is taken from the "description =" parameter in the hardware section of your *radiod* config file, so don't use something generic (e,g. "rx888-wsprdaemon"). Since most of us have more than one antenna, I include it in the description, e.g, "w6lvp loop @ KA9Q".

I am very interested in comments on which method (per channel or per band) seems more efficient and/or works better for you. If there is a clear winner I'll probably remove the other when I next clean up the distribution.


