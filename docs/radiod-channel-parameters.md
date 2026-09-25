# Channel parameters and presets in radiod

A radiod channel is defined by its **current parameters**: demodulator
type (eg, `linear`, `fm`, `wfm`, `spectrum`, `spectrum2`), carrier
frequency, channel count, filter edges, frequency shift, output sample rate, and options such as AM
envelope detection and PLL operation. Controllers set these parameters
through command TLVs and observe their current values through status
TLVs.

## A preset is a command, not a channel property

The `PRESET` command names a group of parameter settings loaded from a
configuration file. Names such as `usb` and `lsb` are convenient
shortcuts for configuring a channel; they do not establish a
persistent operating mode independent of its parameters.

For each incoming command, radiod first scans for a `PRESET` TLV and
applies it, if present, using the definitions stored in the local file
**/usr/share/ka9q-radio/presets.conf**. It then processes the
explicitly specified parameters, allowing them to override the
preset’s settings. This order applies regardless of where the `PRESET`
TLV appears in the command.

Subsequent commands can change those parameters again. For example,
applying `PRESET "usb"` and then setting the filter edges to −3000 and
−50 Hz (even in the same command packet) produces lower-sideband
reception. The earlier preset name no longer accurately describes the channel.

## Status describes the current state

Echoing the last preset name can therefore be misleading. It records a
past configuration action, not necessarily the channel’s present
behavior. For this reason, reporting the last preset is being
deprecated. `PRESET` will remain available as a **write-only command
for backward compatibility**.

Controllers should determine channel state from the reported parameter
values. They should not compare a previously sent preset name or
parameter value with an echoed name or value to decide whether a
command was received.  Since **radiod** ignores invalid commands and
parameters, the only proper way to confirm that a command has been
received is to look for the **tag** field in the command to appear in
at least one status message.

## Channels may have multiple controllers

A difference between requested and reported parameters is not, by
itself, evidence that a command was lost or rejected. Another
controller may have changed the channel in the meantime.

Automatically retransmitting a previous command to restore a
controller’s assumed state can overwrite legitimate changes and cause
controllers to fight each other. Controllers should distinguish an
explicit user request to change the channel from a status update
reporting what the channel currently does.

## Prefer explicit parameter commands

New controllers should avoid `PRESET` and send the parameter settings needed for the requested operation directly. This makes their intent explicit and avoids dependence on the contents of the server’s preset configuration. This can also avoid any ambiguity about the contents of the **presets.conf** file on the system running **radiod**.

A controller can still offer convenient buttons or menu entries named “USB,” “LSB,” or other familiar labels. Selecting one should generate the appropriate parameter TLVs
using a local lookup table, eg, a local copy of **presets.conf**.
Afterward, the controller should always display the reported state, whether or not it matches the requested state, allowing for further changes from any controller.

**The channel’s parameters are authoritative. A preset is only one way to set them.**
