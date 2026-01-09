Using the ka9q-radio command/status protocol
============================================

version 0.1 January 9, 2026
---------------------------

*ka9q-radio* is a multichannel digital downconverter (DDC) software
defined radio (SDR) that uses IP multicasting for its command/status
protocol as well as for all signal output streams. While the IETF
standard RTP (Real Time Protocol) is used for receiver outputs, the
command/status protocol is custom designed.

command/status protocol layout
------------------------------

All commands to the radio daemon *radiod* are contained in UDP
datagrams multicast to an IP multicast group address dedicated to that
instance of *radiod*. The UDP destination port is 5006. The first byte
of each packet after the UDP header denotes whether the packet
contains STATUS (0) or a COMMAND (1); otherwise the formats are
identical. Following the status/command byte are one or more variable
length parameters and values using binary TLV (Type/Length/Value)
encoding. This technique is widely used in core Internet protocols as
it is extensible yet compact and easy to parse.

type-length-value (TLV) encoding and data types
-----------------------------------------------

Each variable length TLV entry consists of at least two bytes. The
first byte gives the meaning of the entry listed in the accompanying
spreadsheet.

The type byte is followed by one or more bytes indicating the length
in bytes of the value to follow. This feature allows a parser to skip
types it doesn't recognize or isn't interested in. Length values from
0 to 127 inclusive indicate that many bytes follow. For values 128
bytes or longer, the high order bit is set to indicate that the lower
7 bits give the number of bytes to follow that contain the actual
length field in big endian order. For example, the byte sequence 82 01
02 indicates a length of 258.

Each parameter type has an implied data type: an unsigned integer, a
32-bit IEEE-754 floating point number, a 64-bit IEEE-754 double
precision floating point number, a UTF-8 text string, or an IPv4 or
IPv6 network socket. Booleans are encoded as the integers 0
(off/no/false) and 1 (on/yes/true).

Each type has specific length requirements.  Integers are encoded
most-significant-byte (MSB) first, with leading zeroes removed. This
permits variable length encodings with arbitrary precision, though no
integers larger than 64 bits (the C uint64_t type) are used in
*ka9q-radio*. Values from 1 to 255 can be encoded with a single byte;
256 to 65,536 with two bytes, and so on.

The number 0 is a special case. An integer, float32 or double64 with
value zero can be expressed with a length field of zero. Since
booleans are encoded as integers, a false value is also sent with a
length of 0.

Strings are variable length; a trailing null is not required since the
length is explicitly given. (C programmers must be sure to append one
when copying strings from a TLV entry.)

Except for zero, a float32 always has length 4 and a double64 always
has length 8. They are sent with IEEE-754 standard binary encoding,
again starting with the most significant byte (ie, the byte containing
the sign and the high 7 bits of the exponent). Except for zero,
floating point values are not shortened like integers.

Sockets contain an IPv4 or IPv6 address followed by a UDP port number,
all in network byte order (MSB first). The address type is distinguished
by the length of the object; a IPv4 socket is 6 bytes (4 bytes for the
address plus 2 bytes for the port) and an IPv6 socket is 18 bytes (16
for the IPv6 address, 2 for the port). Other length values are not
allowed, except that a null socket (address and port all 0's) may be
sent with length zero.

Two types contain list data from the spectrum analyzer. BIN_DATA, the
original format, consists of a list of float32s so the length is a
multiple of 4. BIN_BYTE_DATA consists of a list of unsigned 1-byte
integers (uint8_t) so the length field indicates the number of values
in the list.

using the command/status protocol
---------------------------------

As mentioned above, all command and status packets relating to an
instance of *radiod* are multicast to an IP group address dedicated to
that specific instance. At a minimum, a command packet must contain a
TLV encoding the SSRC (channel ID) plus zero or more additional TLVs
requesting that specific parameters be changed. A command packet with
no parameters beyond the SSRC (and the optional but recommended random
command tag) is a *poll*, and they are all answered in the same way:
with a status packet containing a TLV for every relevant parameter in
the channel's current mode. Not all parameters can be configured his
way, so attempts to change them are simply ignored (though a status
reponse is still sent).

The ordering of TLVs in a command or status packet is arbitrary, though it is recommended to put the important stuff (particularly the SSRC) at the front to ease packet analysis with tools like *Wireshark*.

A single SSRC (channel) may have multiple controllers,
with everyone seeing every command and response. This permits a lot of
versatile applications but care must be taken to avoid inefficiency or
outright conflict. Controllers polling for status should slightly
randomize their polling intervals, restarting their timers whenever
*radiod* responds to another poll so that they can share the
responses. Controllers should be especially careful not to repeat
commands in such a way that a "war" might break out with another
controller. In general, a simple poll should contain only the SSRC
(channel ID) and optional command tag; command packet entries that set
parameters should be included only when they change, eg, when an
operator requests a frequency change.

Parameters are changed by sending a command packet that includes the
TLV of the parameter to be changed. If a parameter is read-only, out
of range or unsupported, *radiod* will respond with a status packet
containing the original value but the command tag field will contain
the tag of the last command packet received, whether or not it
actually changed the channel state. For this reason it is strongly
discouraged to repeatedly resend a command until a requested parameter
changes to the desired value. Simply include a randomly generated tag
and watch for it to appear in a status packet. The same tag will be
repeated in every subsequent status packet until another command with
a dfferent tag is received.

(to be concluded)
