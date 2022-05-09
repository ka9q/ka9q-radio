*ka9q-radio* installation notes
KA9Q, 8 May 2022
===============================

The preferred platform is Debian Linux "bullseye" on the x86-64 and
the 64-bit "bullseye" version of Raspberry Pi OS for the Raspberry Pi
4. (The Raspberry Pi OS is essentially Debian Linux with
customizations.)
Older versions may work, but you may have to fix some problems.

Here's an incomplete list of nits and gotchas I've run into while
installing *ka9q-radio* on various systems.

Set the Locale
--------------

The *control* program wants to display frequencies and other large
numbers with commas between groups of 3 digits for readability, and it
assumes it is doing so when moving the cursor around in the "Tuning"
window.  This won't work right if the default "C" locale is in use. To
fix this, use the appropriate locale for your site; e.g., for the USA,
use "en_US.UTF-8". To set the locale on the Raspberry Pi, run
*raspi-config*, select "Localisation [sic] Options", then "Locale",
and mark the appropriate locale(s). Be sure to set the system default on
the next screen; C.UTF-8 will *not* work.

On a generic Linux system you can set your locale with the
shell environment variable "LANG", e.g.:

>export LANG=en_US.UTF-8

Multicast DNS problems on Ubuntu
--------------------------------

Multicast DNS, which *radiod* relies on, is rather badly broken on
some older versions of Ubuntu, such as 20.04 ("focal"). The symptom
are messages like

>avahi service '2m vertical (2m-vertical-data.local)' successfully established.  
>resolve_mcast getaddrinfo(2m-vertical-data.local,(null)): Temporary failure in name resolution  
>resolve_mcast getaddrinfo(2m-vertical-data.local,(null)): Temporary failure in name resolution

I.e., multicast DNS lookups repeatedly fail even after the name is successfully registered with *avahi*.

The resolver built into systemd is used by default, and multicast
DNS is disabled by default. To enable it, see here:

[How to configure systemd-resolved for mdns multicast dns on local network](
https://unix.stackexchange.com/questions/459991/how-to-configure-systemd-resolved-for-mdns-multicast-dns-on-local-network)

The "mdns4_minimal" resolver ignores multicast addresses (this seems
to have been fixed in later versions). To work around this, edit
*/etc/nsswitch.conf* to use the "mdns4" resolver and create the file
*/etc/mdns.allow* with the entries

>>.local  
>>.local.

to restrict it to the mDNS zone ".local", which is what mdns4_minimal
normally does.








