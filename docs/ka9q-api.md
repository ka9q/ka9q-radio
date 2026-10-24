Using the ka9q-radio command/status protocol
============================================

version 1.0 October 23, 2026
---------------------------

*ka9q-radio* is a multichannel digital downconverter (DDC) software
defined radio (SDR) that uses IP multicasting for both the control
plane (commands and status) and the data plane (signal output
streams).  While the IETF standard RTP (Real Time Protocol) is used
for receiver outputs, the command/status protocol is custom designed.

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

Except for the EOL (type 0) option that ends a message, each variable
length TLV entry consists of at least two bytes. The first byte, the
type, gives the meaning of the entry listed in the accompanying
spreadsheet.

The type byte is followed by one or more bytes indicating the length
in bytes of the value to follow. This feature allows a parser to skip
types it doesn't recognize or isn't interested in. Length values from
0 to 127 inclusive indicate that many bytes follow. For longer
lengths, the high order bit is set to indicate that the lower 7 bits
give the number of bytes to follow that contain the actual length
field in big endian order. For example, the byte sequence 82 01 02
indicates a length of 258.

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
256 to 65,535 with two bytes, and so on.

The number 0 is a special case. An integer, float32 or double64 with
value zero can be expressed with a length field of zero. Since
booleans are encoded as integers, a false value can also be sent with a
length of 0.

Strings are variable length; a trailing null is not required since the
length is explicitly given. (C programmers must be sure to append one
when copying strings from a TLV entry.)

A floating point zero can be encoded just like an integer zero, ie,
with length zero. They are sent with IEEE-754 standard binary
encoding, again starting with the most significant byte (ie, the byte
containing the sign and the high 7 bits of the exponent). As with
integers, leading zeroes can be removed from encoded floating point
numbers but this is unlikely except for very small (denormal)
values. The special IEEE-754 value NAN (Not a Number) is never
encoded, but infinity (positive or negative) is permitted, eg, in a
decibel (dB) field. Except in the deprecated BIN_DATA element from
SPECTRUM version 1 (see below), leading zeroes can be removed from
encoded floating point values just as with integers, but "subnormal"
values that do this are rare. Usually a float32 is 4 bytes and a
double64 is 8 bytes.

Sockets contain an IPv4 or IPv6 address followed by a UDP port number,
all in network byte order (MSB first). The address type is distinguished
by the length of the object; a IPv4 socket is 6 bytes (4 bytes for the
address plus 2 bytes for the port) and an IPv6 socket is 18 bytes (16
for the IPv6 address, 2 for the port). Other length values are not
allowed, except that a null socket (address and port all 0's) may be
sent with length zero.

Two types contain list data from the spectrum analyzer. BIN_DATA, the
original SPECTRUM format, consists of a list of float32s so the length
is a multiple of 4.

BIN_BYTE_DATA consists of a list of unsigned 1-byte
integers (uint8_t) so the length field indicates the number of values
in the list.

The end of a command/status message always ends with the special EOL
command, type 0. It has no length or value field.


using the command/status protocol
---------------------------------

All command and status packets are sent to an IP multicast group
address dedicated to each *radiod* instance.  At a minimum, a command
must contain a TLV encoding the SSRC (channel ID) plus zero or more
additional TLVs requesting that specific parameters be changed. Every
command received by *radiod* elicits a status packet with the full
channel state even if the command packet contains no valid commands
and causes no change in channel state. (A command packet containing
only the SSRC is called a *poll*.)

If a command includes a special TLV called the *tag* with an arbitrary
integer, *radiod* echoes it in every status message until a new tag is
received. Every command generates its own status reply, so a tag is
always echoed at least once if it was received at all. This is how
*radiod* acknowledges commands even when they do not change channel
state. If a controller does not see its tag echoed within a reasonable
time, it may retransmit its command with either the old or a new
tag. Commands are idempotent, ie, repeating a command causes no
further change in channel state.

Except for the tag, there are no sequence numbers. It is assumed that
while the network might occasionally drop a packet, it delivers
packets in order. Any conflict between multiple controllers on the
same channel must be handled administratively.

Channel threads synchronously process and answer their own commands.
The command is processed, then the data for the current frame, and
finally a response is generated (if there was a command).  To limit
latency, each channel processes at most one command per frame. This
also ensures that every command tag is echoed at least once, ie,
responses are not combined.

Controllers may periodically send polls (empty commands) to refresh
their local copies of the channel state.

The ordering of TLVs in a command or status packet is arbitrary,
though it is recommended to put the important stuff (particularly the
SSRC) at the front to ease packet analysis with tools like
*Wireshark*.

A single SSRC (channel) may have multiple controller, with everyone
seeing every command and response. This is highly versatile but care
must be taken to avoid inefficiency or outright conflict. Controllers
polling for status should send *only* the SSRC and tag and slightly
randomize their poll timers, restarting whenever they see a status
message regardless of what triggered it.  *Controllers must not
blindly repeat commands in such a way that a "war" might break out
with another controller*.  TLVs that set parameters should be sent
only when they change, eg, when an operator requests a frequency
change.

