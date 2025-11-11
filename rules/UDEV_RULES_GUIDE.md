# USB Device Permission Rules for SDR Hardware

## Overview

The `rules/` directory contains configuration files for managing USB device permissions on different operating systems. These rules allow non-root users to access SDR (Software Defined Radio) hardware.

## Purpose

By default, USB devices require root privileges to access. These rules:
- Grant normal users permission to access SDR devices
- Create convenient device symlinks (e.g., `/dev/airspy-0`)
- Assign devices to specific groups (typically `plugdev`)
- Simplify device management

## Installation

### Using CMake (Recommended)

```bash
# Build and install (requires root for system directories)
cmake -B build
sudo cmake --install build

# Reload udev rules (Linux only)
sudo udevadm control --reload-rules
sudo udevadm trigger

# Or use the convenience target
sudo cmake --build build --target udev-reload

# Add yourself to the plugdev group
sudo usermod -a -G plugdev $USER

# Log out and back in for group changes to take effect
```

### Manual Installation (Linux)

```bash
sudo cp rules/*.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger
sudo usermod -a -G plugdev $USER
# Log out and back in
```

## Supported Devices

The following SDR devices are covered by these rules:

| Device | Rule File | Vendor ID | Product ID |
|--------|-----------|-----------|------------|
| **Airspy R2/Mini** | `52-airspy.rules` | 1d50 | 60a1 |
| **Airspy HF+** | `52-airspyhf.rules` | 03eb | 800c |
| **RTL-SDR** | `20-rtlsdr.rules` | Multiple | Multiple |
| **HackRF One** | `66-hackrf.rules` | 1d50 | 6089 |
| **Funcube Dongle** | `68-funcube-dongle.rules` | 04d8 | fb56 |
| **Funcube Dongle Pro+** | `68-funcube-dongle-proplus.rules` | 04d8 | fb31 |
| **Fobos SDR** | `73-fobos-sdr.rules` | 0403 | 6014, 6015 |
| **RX888** | `99-rx888.rules` | Multiple | Multiple |

## Platform-Specific Instructions

### Linux (udev)

✅ **Fully Supported**

#### Verify Installation

```bash
# Check if rules are installed
ls -l /etc/udev/rules.d/*sdr*.rules /etc/udev/rules.d/*airspy*.rules

# Check if you're in the plugdev group
groups | grep plugdev

# Test device access (plug in your SDR)
ls -l /dev/airspy* /dev/hackrf* /dev/rtlsdr*
```

#### Troubleshooting

**Device not accessible:**
```bash
# Check if the device is detected
lsusb | grep -i "airspy\|hackrf\|rtl\|funcube"

# Check udev rules are loaded
udevadm test $(udevadm info -q path -n /dev/bus/usb/001/002) 2>&1 | grep -i rules

# Check permissions
ls -l /dev/bus/usb/001/002  # Replace with your device path

# Manually trigger udev
sudo udevadm control --reload-rules
sudo udevadm trigger

# Verify group membership took effect
id | grep plugdev
# If not shown, log out and back in
```

**Still not working:**
```bash
# Check if plugdev group exists
getent group plugdev

# If not, create it
sudo groupadd plugdev

# Add yourself to the group
sudo usermod -a -G plugdev $USER

# Unplug and replug the device
# Or: sudo udevadm trigger
```

#### Understanding udev Rules

Example from `52-airspy.rules`:
```
ATTR{idVendor}=="1d50", ATTR{idProduct}=="60a1", SYMLINK+="airspy-%k", MODE="660", GROUP="plugdev"
```

Breaking it down:
- `ATTR{idVendor}=="1d50"` - Matches USB vendor ID (Airspy)
- `ATTR{idProduct}=="60a1"` - Matches USB product ID (Airspy R2/Mini)
- `SYMLINK+="airspy-%k"` - Creates symlink like `/dev/airspy-0`
- `MODE="660"` - Sets permissions (owner and group can read/write)
- `GROUP="plugdev"` - Sets group ownership to plugdev

### macOS

ℹ️ **Not Applicable**

macOS doesn't use udev. USB devices are typically accessible to all users by default.

#### If You Encounter Permission Issues

```bash
# Check USB devices
system_profiler SPUSBDataType

# Check which process is using the device
sudo lsof | grep usb

# Reset USB system (unplug/replug device)

# Check System Settings
# System Settings → Privacy & Security → USB
```

Most SDR applications on macOS will work without any special configuration.

### FreeBSD

⚠️ **TODO: devd Support Not Yet Implemented**

FreeBSD uses `devd` instead of `udev` for device management.

#### Current Workaround

```bash
# Add yourself to the usb group
sudo pw groupmod usb -m $USER

# Log out and back in

# Check group membership
id | grep usb

# Device should now be accessible
```

#### Future Implementation (devd Rules)

When devd support is added, rules will be installed to `/usr/local/etc/devd/` and look like:

```
attach 100 {
    match "vendor" "0x1d50";
    match "product" "0x60a1";
    action "chmod 660 /dev/$device-name";
    action "chown root:usb /dev/$device-name";
};
```

## Testing Device Access

### Verify Permissions

```bash
# Linux: List USB devices with vendor/product IDs
lsusb

# Find your device in the output, note bus and device numbers
# Example: Bus 001 Device 005: ID 1d50:60a1 OpenMoko, Inc.

# Check permissions on that device
ls -l /dev/bus/usb/001/005

# Should show:
# crw-rw---- 1 root plugdev ... /dev/bus/usb/001/005
#          ^^^      ^^^^^^^
#          User and group have read/write
```

### Test with SDR Software

```bash
# RTL-SDR test
rtl_test -t

# Airspy test
airspy_info

# HackRF test
hackrf_info

# If these work without sudo, permissions are correct!
```

## Common Issues and Solutions

### Issue: "Permission denied" when accessing device

**Solution:**
1. Verify you're in the plugdev group: `groups | grep plugdev`
2. If not, add yourself: `sudo usermod -a -G plugdev $USER`
3. **Log out and back in** (group membership changes require new login)
4. Reload udev: `sudo udevadm control --reload-rules && sudo udevadm trigger`
5. Unplug and replug the device

### Issue: udev rules not taking effect

**Solution:**
```bash
# Check if rules file exists
ls -l /etc/udev/rules.d/52-airspy.rules

# Check for syntax errors
udevadm test $(udevadm info -q path -n /dev/bus/usb/001/005) 2>&1 | grep ERROR

# Force reload
sudo udevadm control --reload-rules
sudo udevadm trigger --action=change

# Or restart udev service
sudo systemctl restart systemd-udevd
```

### Issue: Device shows up but software can't find it

**Solution:**
1. Check if symlink was created: `ls -l /dev/airspy*`
2. Check if device is claimed by kernel driver:
   ```bash
   lsusb -t  # Look for your device
   # If it shows "Driver=dvb_usb_rtl28xxu", kernel driver is claiming it
   ```
3. Blacklist kernel driver if needed:
   ```bash
   echo "blacklist dvb_usb_rtl28xxu" | sudo tee /etc/modprobe.d/blacklist-rtl-sdr.conf
   sudo modprobe -r dvb_usb_rtl28xxu
   ```

### Issue: Multiple devices of the same type

**Solution:**
The `%k` in the SYMLINK creates unique device names:
```bash
ls -l /dev/airspy*
# Shows: airspy-0, airspy-1, etc.
```

## Security Considerations

### Why plugdev Group?

The `plugdev` group is a standard Linux group for users who should be able to access removable devices. It's less privileged than `root` but more than regular users.

### Alternative: Per-User Rules

If you want more restrictive access, you can modify the rules to match your specific user:

```bash
# In the .rules file, replace:
# GROUP="plugdev"
# With:
# OWNER="yourusername"

# Then only you can access the device
```

### Audit Device Access

```bash
# Check who's accessing USB devices
sudo lsof /dev/bus/usb/*/*

# Check udev events
sudo udevadm monitor --environment --udev
```

## Creating Custom Rules

### For a New SDR Device

1. **Find the device IDs:**
   ```bash
   lsusb
   # Look for your device, note vendor and product IDs
   ```

2. **Create a rule file:**
   ```bash
   sudo nano /etc/udev/rules.d/52-mydevice.rules
   ```

3. **Add the rule:**
   ```
   ATTR{idVendor}=="1234", ATTR{idProduct}=="5678", SYMLINK+="mydevice-%k", MODE="660", GROUP="plugdev"
   ```

4. **Reload udev:**
   ```bash
   sudo udevadm control --reload-rules
   sudo udevadm trigger
   ```

5. **Test:**
   ```bash
   ls -l /dev/mydevice*
   ```

### Rule File Naming

The number prefix determines loading order:
- `20-*` - Early rules (storage, basic devices)
- `50-*` - Default rules
- `60-*` - SDR devices
- `70-*` - Other peripherals
- `99-*` - Late rules (catch-all)

## Resources

### Documentation
- **udev (Linux)**: https://www.reactivated.net/writing_udev_rules.html
- **devd (FreeBSD)**: https://www.freebsd.org/cgi/man.cgi?query=devd.conf
- **USB IDs Database**: https://www.linux-usb.org/usb-ids.html

### Finding Device Information
```bash
# Linux: Detailed USB info
lsusb -v

# List device attributes (useful for writing rules)
udevadm info --attribute-walk --path=$(udevadm info --query=path --name=/dev/bus/usb/001/005)

# Monitor udev events in real-time
sudo udevadm monitor --environment --udev

# FreeBSD: USB device info
usbconfig list

# macOS: USB device info
system_profiler SPUSBDataType
```

## Contributing

When adding rules for new devices:

1. Test thoroughly on your system
2. Verify the vendor and product IDs are correct
3. Use consistent formatting (see existing files)
4. Add comments explaining the device
5. Update this documentation
6. Test that the rule works after system reboot

## Summary

- **Linux**: Rules installed to `/etc/udev/rules.d/`, reload with `udevadm`
- **macOS**: No special configuration needed
- **FreeBSD**: Add user to `usb` group (devd rules coming soon)
- **Group**: Must be in `plugdev` (Linux) or `usb` (FreeBSD) group
- **Reload**: Always reload rules and replug device after changes
- **Login**: Must log out/in after being added to a group

After following these instructions, you should be able to access SDR devices as a regular user without needing sudo!
