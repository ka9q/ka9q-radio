# System Configuration Guide (aux/ Directory)

## Overview

The `aux/` directory contains system-level configuration files that integrate ka9q-radio with your operating system. These files handle:

- **User/Group Management** - Creates the `radio` system user and group
- **Directory Structure** - Creates runtime and data directories
- **Kernel Tuning** - Optimizes system parameters for SDR performance
- **Log Management** - Configures automatic log rotation
- **Module Management** - Prevents kernel driver conflicts
- **Maintenance** - Automated cleanup tasks
- **Utility Scripts** - Helper scripts for common tasks

⚠️ **WARNING:** These files require root privileges and modify system configuration!

---

## Quick Start (Linux)

```bash
# Build and install
cmake -B build
sudo cmake --install build

# Run post-install setup (combines all the steps below)
sudo cmake --build build --target system-setup

# OR run steps manually:
sudo systemd-sysusers          # Create radio user/group
sudo systemd-tmpfiles --create # Create directories
sudo sysctl --system           # Apply kernel tuning

# Add yourself to the radio group
sudo usermod -a -G radio $USER

# Log out and back in for group changes to take effect
```

---

## Installation Details

### What Gets Installed (Linux)

```
/etc/
├── sysusers.d/
│   └── radio.conf              # Creates radio user (UID 55050)
├── tmpfiles.d/
│   └── ka9q-radio.conf         # Creates directories
├── sysctl.d/
│   ├── 50-multicast.conf       # Enables multicast routing
│   └── 98-sockbuf.conf         # Increases socket buffers
├── modprobe.d/
│   └── airspy-blacklist.conf   # Blacklists conflicting drivers
├── logrotate.d/
│   ├── aprsfeed.rotate         # Log rotation configs
│   ├── ft4.rotate
│   ├── ft8.rotate
│   ├── hfdl.rotate
│   └── wspr.rotate
└── cron.d/
    └── ka9q-cleanups           # Periodic cleanup tasks

/usr/local/
├── bin/
│   └── set_lo_multicast.sh     # Multicast setup script
└── sbin/
    ├── start-hfdl.sh           # HFDL startup script
    └── start-ka9q-horus.sh     # Horus balloon decoder

/var/lib/hfdl/
└── systable.conf               # HFDL ground station table

/var/lib/ka9q-radio/            # Created by tmpfiles.d
/var/log/ka9q-radio/            # Created by tmpfiles.d
/run/ka9q-radio/                # Created by tmpfiles.d
```

---

## System Components Explained

### 1. User and Group (sysusers.d)

**File:** `radio.sysusers` → `/etc/sysusers.d/radio.conf`

Creates a system user and group for running ka9q-radio daemons:
- **User:** `radio` (UID 55050)
- **Group:** `radio` (GID 55050)
- **Home:** `/var/lib/ka9q-radio`
- **Shell:** `/usr/sbin/nologin` (no interactive login)

**Why this matters:**
- Daemons run as `radio` user, not root (security)
- Files owned by `radio:radio` are protected
- Users in `radio` group can access data files

**Manual creation (if systemd-sysusers not available):**
```bash
sudo groupadd -g 55050 radio
sudo useradd -u 55050 -g radio -d /var/lib/ka9q-radio -s /usr/sbin/nologin radio
```

### 2. Directory Management (tmpfiles.d)

**File:** `ka9q-radio.tmpfiles` → `/etc/tmpfiles.d/ka9q-radio.conf`

Creates required directories with proper ownership:
```
/var/lib/ka9q-radio  - Data storage (FT8/FT4/WSPR spots, recordings, etc.)
/var/log/ka9q-radio  - Log files
/run/ka9q-radio      - Runtime files (PID files, sockets)
```

**Permissions:** `0750` (owner + group read/write/execute, no world access)

**Manual creation:**
```bash
sudo mkdir -p /var/lib/ka9q-radio /var/log/ka9q-radio /run/ka9q-radio
sudo chown -R radio:radio /var/lib/ka9q-radio /var/log/ka9q-radio /run/ka9q-radio
sudo chmod -R 750 /var/lib/ka9q-radio /var/log/ka9q-radio /run/ka9q-radio
```

### 3. Kernel Parameter Tuning (sysctl.d)

**Files:**
- `98-sockbuf.conf` - Increases network socket buffers
- `50-multicast.conf` - Enables multicast routing

#### 98-sockbuf.conf
```
net.core.rmem_max = 5242880      # Max receive buffer (5 MB)
net.core.rmem_default = 5242880  # Default receive buffer
net.core.wmem_max = 5242880      # Max send buffer
net.core.wmem_default = 5242880  # Default send buffer
```

**Why this matters:**
High-speed SDR data streams require large buffers to prevent packet loss.

**Manual application:**
```bash
sudo sysctl -w net.core.rmem_max=5242880
sudo sysctl -w net.core.wmem_max=5242880
# etc.

# Or reload all sysctl configs:
sudo sysctl --system
```

**Verify settings:**
```bash
sysctl net.core.rmem_max net.core.wmem_max
```

#### 50-multicast.conf
Enables IP multicast routing on the loopback interface for local multicast communication between ka9q-radio components.

### 4. Kernel Module Blacklist (modprobe.d)

**File:** `airspy-blacklist.conf`

Prevents the kernel DVB driver from claiming Airspy devices:
```
# Blacklist DVB driver that conflicts with Airspy userspace driver
blacklist dvb_usb_airspy
```

**Why this matters:**
The kernel has built-in DVB drivers that claim Airspy devices before userspace software can. Blacklisting prevents this conflict.

**Manual blacklist:**
```bash
echo "blacklist dvb_usb_airspy" | sudo tee /etc/modprobe.d/airspy-blacklist.conf
sudo modprobe -r dvb_usb_airspy  # Unload if already loaded
sudo update-initramfs -u          # Debian/Ubuntu
# OR
sudo dracut -f                    # Fedora/RHEL
```

### 5. Log Rotation (logrotate.d)

**Files:** `*.rotate` files for various daemons

Example (`aprsfeed.rotate`):
```
/var/log/aprsfeed.log {
    daily
    rotate 7
    compress
    missingok
    notifempty
}
```

**What it does:**
- Rotates log files daily
- Keeps 7 days of logs
- Compresses old logs
- Doesn't complain if log file is missing
- Doesn't rotate empty logs

**Manual rotation:**
```bash
sudo logrotate /etc/logrotate.d/aprsfeed.rotate
```

**Test configuration:**
```bash
sudo logrotate -d /etc/logrotate.d/aprsfeed.rotate
```

### 6. Scheduled Maintenance (cron.d)

**File:** `ka9q-cleanups`

Periodic cleanup tasks to prevent disk space issues:
```
# Clean up old FT8/FT4/WSPR spots
0 0 * * * radio find /var/lib/ka9q-radio/{ft8,ft4,wspr} -type f -mtime +30 -delete
```

**What it does:**
- Runs daily at midnight
- Deletes files older than 30 days
- Runs as `radio` user

**Manual cleanup:**
```bash
sudo -u radio find /var/lib/ka9q-radio/ft8 -type f -mtime +30 -delete
```

### 7. Utility Scripts

#### set_lo_multicast.sh
Sets up local multicast routing on the loopback interface.

**Usage:**
```bash
sudo set_lo_multicast.sh
```

**What it does:**
```bash
ip route add 239.0.0.0/8 dev lo
```

Adds multicast route for 239.0.0.0/8 (local multicast) to loopback interface.

#### start-hfdl.sh
Starts the HFDL (High Frequency Data Link) decoder daemon.

**Usage:**
```bash
sudo start-hfdl.sh
```

#### start-ka9q-horus.sh
Starts the Horus high-altitude balloon telemetry decoder.

**Usage:**
```bash
sudo start-ka9q-horus.sh
```

### 8. HFDL Configuration

**File:** `systable.conf`

HFDL ground station frequency table. Lists frequencies and times for each HFDL ground station worldwide.

**Installation:**
Only installed if file doesn't already exist (preserves user modifications).

**Location:** `/var/lib/hfdl/systable.conf`

---

## Platform-Specific Instructions

### Linux (Full Automation)

```bash
# Install
sudo cmake --install build

# Run setup
sudo cmake --build build --target system-setup

# Add yourself to radio group
sudo usermod -a -G radio $USER

# Log out and back in

# Verify setup
groups | grep radio                        # Check group membership
ls -ld /var/lib/ka9q-radio                # Check directory
sysctl net.core.rmem_max                  # Check kernel tuning
systemctl status radio                     # Check if user exists
```

### macOS (Manual Setup)

```bash
# Install scripts
sudo cmake --install build

# Create radio group
sudo dscl . -create /Groups/radio
sudo dscl . -create /Groups/radio PrimaryGroupID 492

# Create radio user (optional, for daemon usage)
sudo dscl . -create /Users/radio
sudo dscl . -create /Users/radio UniqueID 492
sudo dscl . -create /Users/radio PrimaryGroupID 492
sudo dscl . -create /Users/radio UserShell /usr/bin/false
sudo dscl . -create /Users/radio NFSHomeDirectory /var/lib/ka9q-radio

# Create directories
sudo mkdir -p /var/lib/ka9q-radio /var/log/ka9q-radio
sudo chown -R radio:radio /var/lib/ka9q-radio /var/log/ka9q-radio
sudo chmod -R 750 /var/lib/ka9q-radio /var/log/ka9q-radio

# Add yourself to radio group
sudo dscl . -append /Groups/radio GroupMembership $USER

# Log out and back in
```

**Note:** macOS doesn't use systemd, so sysusers.d and tmpfiles.d don't apply.

### FreeBSD (Manual Setup)

```bash
# Install scripts
sudo cmake --install build

# Create radio group
sudo pw groupadd radio -g 55050

# Create radio user
sudo pw useradd radio -u 55050 -g radio \
  -d /var/lib/ka9q-radio -s /usr/sbin/nologin \
  -c "ka9q-radio system user"

# Create directories
sudo mkdir -p /var/lib/ka9q-radio /var/log/ka9q-radio
sudo chown -R radio:radio /var/lib/ka9q-radio /var/log/ka9q-radio
sudo chmod -R 750 /var/lib/ka9q-radio /var/log/ka9q-radio

# Add kernel tuning to /etc/sysctl.conf
sudo tee -a /etc/sysctl.conf <<EOF
kern.ipc.maxsockbuf=5242880
net.inet.udp.recvspace=5242880
EOF

# Apply sysctl changes
sudo sysctl -f /etc/sysctl.conf

# Add yourself to radio group
sudo pw groupmod radio -m $USER

# Log out and back in
```

---

## Troubleshooting

### Issue: Permission denied accessing /var/lib/ka9q-radio

**Solution:**
```bash
# Check ownership
ls -ld /var/lib/ka9q-radio

# Should show: drwxr-x--- radio radio

# Fix ownership
sudo chown -R radio:radio /var/lib/ka9q-radio

# Check group membership
groups | grep radio

# Add to group if needed
sudo usermod -a -G radio $USER
# Log out and back in!
```

### Issue: Socket buffer size too small (packet loss)

**Solution:**
```bash
# Check current values
sysctl net.core.rmem_max net.core.wmem_max

# Should be: 5242880

# If not, apply manually
sudo sysctl -w net.core.rmem_max=5242880
sudo sysctl -w net.core.wmem_max=5242880

# Make permanent
sudo sysctl --system
```

### Issue: Airspy device claimed by kernel driver

**Solution:**
```bash
# Check if DVB driver is loaded
lsmod | grep dvb_usb_airspy

# If loaded, unload it
sudo modprobe -r dvb_usb_airspy

# Check blacklist
cat /etc/modprobe.d/airspy-blacklist.conf

# Should contain: blacklist dvb_usb_airspy

# If not, add it
echo "blacklist dvb_usb_airspy" | sudo tee /etc/modprobe.d/airspy-blacklist.conf

# Update initramfs
sudo update-initramfs -u  # Debian/Ubuntu
# OR
sudo dracut -f            # Fedora/RHEL

# Reboot
sudo reboot
```

### Issue: Logs filling up disk space

**Solution:**
```bash
# Check log sizes
du -sh /var/log/ka9q-radio/*

# Force log rotation
sudo logrotate -f /etc/logrotate.d/*

# Test rotation config
sudo logrotate -d /etc/logrotate.d/aprsfeed.rotate

# Clean up old logs manually
sudo find /var/log/ka9q-radio -name "*.gz" -mtime +7 -delete
```

### Issue: radio user doesn't exist

**Solution:**
```bash
# On Linux with systemd:
sudo systemd-sysusers

# Manually:
sudo groupadd -g 55050 radio
sudo useradd -u 55050 -g radio -d /var/lib/ka9q-radio -s /usr/sbin/nologin radio

# Verify
id radio
```

---

## Security Considerations

### Why Run as radio User?

**Principle of Least Privilege:**
- Daemons don't need root privileges for normal operation
- Compromised daemon can't access other system resources
- File ownership prevents accidental deletion/modification

### Directory Permissions

**0750 (rwxr-x---):**
- Owner (radio): Full access
- Group (radio): Read and execute
- Others: No access

Users must be in `radio` group to access data files.

### Script Permissions

Scripts are installed with execute permission but can only modify files owned by `radio` user.

---

## Advanced Configuration

### Custom UID/GID

If UID/GID 55050 conflicts with your system:

1. Edit `radio.sysusers`:
   ```
   g radio 55051
   u radio 55051 "ka9q-radio system" /var/lib/ka9q-radio /usr/sbin/nologin
   ```

2. Reinstall:
   ```bash
   sudo cmake --install build
   sudo systemd-sysusers
   ```

### Custom Directories

Edit `ka9q-radio.tmpfiles` to change directory locations or permissions.

### Custom Socket Buffers

Edit `98-sockbuf.conf` to adjust buffer sizes for your specific hardware.

---

## Uninstallation

To remove system configuration:

```bash
# Remove configuration files
sudo rm -f /etc/sysusers.d/radio.conf
sudo rm -f /etc/tmpfiles.d/ka9q-radio.conf
sudo rm -f /etc/sysctl.d/50-multicast.conf
sudo rm -f /etc/sysctl.d/98-sockbuf.conf
sudo rm -f /etc/modprobe.d/airspy-blacklist.conf
sudo rm -f /etc/logrotate.d/{aprsfeed,ft4,ft8,hfdl,wspr}.rotate
sudo rm -f /etc/cron.d/ka9q-cleanups

# Remove user and group
sudo userdel radio
sudo groupdel radio

# Remove directories (careful - this deletes data!)
sudo rm -rf /var/lib/ka9q-radio
sudo rm -rf /var/log/ka9q-radio
sudo rm -rf /run/ka9q-radio

# Revert sysctl changes
sudo sysctl -w net.core.rmem_max=212992
sudo sysctl -w net.core.wmem_max=212992
```

---

## Summary

The aux/ directory provides complete system integration for ka9q-radio:

- ✅ **Linux**: Fully automated with systemd
- ⚠️ **macOS**: Manual setup required
- ⚠️ **FreeBSD**: Manual setup required

After installation and setup, you'll have:
- A dedicated `radio` user and group
- Properly configured directories
- Optimized kernel parameters
- Automatic log management
- Maintenance automation

All designed for secure, efficient SDR operation!
