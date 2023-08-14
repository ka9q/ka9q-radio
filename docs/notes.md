*ka9q-radio* installation notes  
13 August 2023
===============================

The preferred platform is Debian Linux 12 ("bookworm") on the x86-64 and
the 64-bit "bullseye" version of Raspberry Pi OS for the Raspberry Pi
4. (The Raspberry Pi OS is essentially Debian Linux 11 with
customizations. As of this writing it has not yet incorporated Debian version 12.)
Older versions may work, but you may have to fix some problems.

Front end drivers now merged into *radiod*

*ka9q-radio* was substantially restructured during the summer of 2023 to merge the front end
drivers into *radiod*. The drivers are no longer separate programs using multicast IP to communicate
with *radiod*. This considerably improves performance and simplifies configuration; what used to be a separate config file
(e.g., */etc/radio/airspyd.conf*) is now a section in the *radiod* configuration file. The separate drivers
(*airspyd*, etc) are gone.

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

The problem is that the "mdns4_minimal" resolver specified in
/etc/nsswitch.conf ignores multicast addresses (this seems to have
been fixed in later versions). To work around this, edit
*/etc/nsswitch.conf* to change "mdns4_minimal" to "mdns4". Then create
the file */etc/mdns.allow* with the entries

>>.local  
>>.local.

so it will only try to resolve names in the mDNS zone ".local", which
is what mdns4_minimal normally does.








