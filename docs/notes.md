*ka9q-radio* installation notes
KA9Q, 8 May 2022
===============================

An incomplete list of nits and gotchas I've run into while installing
*ka9q-radio* on various systems:

The preferred platform is Debian Linux "bullseye" on the x86-64 and
the 64-bit "bullseye" version of Raspberry Pi OS for the Raspberry Pi
4. (The Raspberry Pi OS is essentially Debian Linux with
customizations.)

Multicast DNS problems on Ubuntu
--------------------------------

Older versions may work, but you may have to fix some problems. For
example, multicast DNS, which *radiod* uses heavily, is rather badly
broken on some older versions of Ubuntu, such as 20.04
("focal"). The symptoms of this problem will be messages like

>avahi service '2m vertical (2m-vertical-data.local)' successfully established.  
>resolve_mcast getaddrinfo(2m-vertical-data.local,(null)): Temporary failure in name resolution  
>resolve_mcast getaddrinfo(2m-vertical-data.local,(null)): Temporary failure in name resolution

I.e., multicast DNS lookups repeatedly fail even after the name is successfully registered with *avahi*.

Here's a (probably incomplete) list of fixes:

1. The "mdns4_minimal" resolver ignores multicast addresses (this seems
to have been fixed in later versions). To work around this, edit
*/etc/nsswitch.conf* to use the "mdns4" resolver and create the file
*/etc/mdns.allow* with the entries

>.local  
>.local.

to restrict it to the mDNS zone ".local".

2. The resolver built into systemd is used by default, and multicast
DNS is disabled by default. To enable it, see here:

[How to configure systemd-resolved for mdns multicast dns on local network](
https://unix.stackexchange.com/questions/459991/how-to-configure-systemd-resolved-for-mdns-multicast-dns-on-local-network)





