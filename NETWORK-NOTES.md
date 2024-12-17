# Some useful notes on networking for 'ka9q-radio'

By default multicast packets are sent and received over the primary interface, which is typically the wired network or the wireless network. To see which interface is the primary one, run:
```
ip address show
```
and look for the first interface marked 'MULTICAST', 'UP', and 'LOWER_UP'

For a full local setup where both 'radiod' and the client applications ('monitor', 'control', 'pcmcat', etc) are on the same host, the loopback ('lo') interface can be used instead as documented in the second part of the document.


## Using the primary interface for multicasting

This is the default configuration with 'ka9q-radio'; the important task in this case is to create firewall rules for the RTP streams (data) and for the control application


### Firewall rule for RTP streams

The RTP streams (sound/IQ data) use port 5004/udp; for client applications like 'monitor' and 'pcmcat' to work, port 5004/udp needs to be open:
```
firewall-cmd --add-port=5004/udp --permanent
firewall-cmd --reload
```

The 'monitor' client application can then be used to listen to the RTP streams; for instance:
```
monitor nws-pcm.local
```

A tighter firewall rule allowing only multicast addresses in the 239.0.0.0/8 network can be defined (instead of the one above) using firewalld 'rich rules':
```
firewall-cmd --add-rich-rule='rule family="ipv4" destination address="239.0.0.0/8" port port="5004" protocol="udp" accept' --permanent
firewall-cmd --reload
```


### Firewall rule for control connection

The control connection instead uses port 5006/udp; for client applications like 'control' to work, port 5006/udp needs to be open:
```
firewall-cmd --add-port=5006/udp --permanent
firewall-cmd --reload
```

The 'control' client application can then be used to send control commands to a specific channel in 'radiod' identified by its SSRC; for instance:
```
control -s 162550 nws.local
```

A tighter firewall rule allowing only multicast addresses in the 239.0.0.0/8 network can be defined (instead of the one above) using firewalld 'rich rules':
```
firewall-cmd --add-rich-rule='rule family="ipv4" destination address="239.0.0.0/8" port port="5006" protocol="udp" accept' --permanent
firewall-cmd --reload
```


## Using the loopback interface for multicasting

This configuration can be used when 'radiod' and all the client applications run on the same host - it will not send multicast packets over to any external network (thus possibly avoiding problems when those networks cannot support multicasting).

The first step is to add the following four lines to 'multicast.c', rebuild and reinstall 'ka9q-radio':
```
diff --git a/multicast.c b/multicast.c
index 1c451b6..a1f3667 100644
--- a/multicast.c
+++ b/multicast.c
@@ -717,6 +717,10 @@ static int ipv4_join_group(int const fd,void const * const sock,char const * con
     mreqn.imr_ifindex = 0;
   else
     mreqn.imr_ifindex = if_nametoindex(iface);
+  if(setsockopt(fd,IPPROTO_IP,IP_MULTICAST_IF,&mreqn,sizeof(mreqn)) != 0){
+    perror("multicast v4 set interface");
+    return -1;
+  }
   if(setsockopt(fd,IPPROTO_IP,IP_ADD_MEMBERSHIP,&mreqn,sizeof(mreqn)) != 0){
     perror("multicast v4 join");
     return -1;
```
A possible reason for having to call setsockopt(IP_MULTICAST_IF) is the use of the 'connect()' function, as explained here: https://stackoverflow.com/questions/31791958/why-does-a-udp-connect-call-ignore-a-loopback-multicast-route-but-a-sendto-witho

The next step is to add the line 'iface = lo' in the [global] section in the radiod configuration file being run (and restart the corresponding radiod service); for instance:
```
[global]
samprate = 24000
status = nws.local
hardware = sdrplay
#verbose = 1
iface = lo
```


Please note that this configuration does not require any additional firewall rules (since all the multicast traffic is over the loopback interface)


I also found that I didn't to add a routing table rule to send multicast over the loopback interface ('ip route add 239.0.0.0/8 dev lo') and I didn't have to explicitly enable multicasting on the loopback interface ('ip link set lo multicast on'); I thought I would mention these two commands in case someone else needs them.


Finally in order to run 'monitor', 'control', 'pcmcat', and other client applications using the loopback interface, you have to append ',lo' to the mDNS name; for instance:
```
monitor nws-pcm.local,lo
```
or:
```
control -s 162550 nws.local,lo
```
