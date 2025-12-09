# Systemd Service Files for ka9q-radio

## Overview

The `service/` directory contains systemd service files for running ka9q-radio daemons and related programs. These allow automatic startup, restart on failure, logging, and easy management via `systemctl`.

## Installation

### Using CMake (Recommended)

```bash
# Build and install (requires root for system directories)
cmake -B build
cmake --build build
sudo cmake --install build

# Reload systemd to recognize new services
sudo systemctl daemon-reload

# Or use the convenience target
sudo cmake --build build --target systemd-reload
```

### Manual Installation

If you prefer to install service files manually:

```bash
sudo cp service/*.service /etc/systemd/system/
sudo systemctl daemon-reload
```

## Available Services

### Core Services

#### radiod@.service (Template Service)
Radio daemon that controls SDR hardware. This is a template service - you can run multiple instances with different configuration files.

```bash
# Start radiod with /etc/radio/config1.conf
sudo systemctl start radiod@config1.service
sudo systemctl enable radiod@config1.service

# Start another instance with /etc/radio/config2.conf
sudo systemctl start radiod@config2.service
```

#### packetd.service
Packet radio decoder daemon.

```bash
sudo systemctl start packetd.service
sudo systemctl enable packetd.service
```

#### aprsfeed.service
APRS feed daemon for connecting to APRS-IS network.

```bash
sudo systemctl start aprsfeed.service
sudo systemctl enable aprsfeed.service
```

#### cwd.service
CW (Morse code) decoder daemon.

```bash
sudo systemctl start cwd.service
sudo systemctl enable cwd.service
```

### Digital Mode Decoders

#### FT8 Services
FT8 is a popular weak-signal digital mode.

```bash
# FT8 recorder
sudo systemctl start ft8-record.service

# FT8 decoder (single instance)
sudo systemctl start ft8-decode.service

# FT8 decoder (template - multiple instances)
sudo systemctl start ft8-decode@band1.service
sudo systemctl start ft8-decode@band2.service
```

#### FT4 Services
FT4 is similar to FT8 but optimized for contesting.

```bash
# FT4 recorder
sudo systemctl start ft4-record.service

# FT4 decoder
sudo systemctl start ft4-decode.service

# FT4 decoder (template)
sudo systemctl start ft4-decode@20m.service
```

#### WSPR Service
WSPR (Weak Signal Propagation Reporter) decoder.

```bash
sudo systemctl start wspr-decoded.service
sudo systemctl enable wspr-decoded.service
```

### Aviation Services

#### ACARS Service
Aircraft Communications Addressing and Reporting System decoder.

```bash
sudo systemctl start acars.service
sudo systemctl enable acars.service
```

#### HFDL Service
High Frequency Data Link decoder for HF aviation communications.

```bash
sudo systemctl start hfdl.service
sudo systemctl enable hfdl.service
```

### Special Services

#### horusdemod.service
Decoder for Horus high altitude balloon telemetry.

```bash
sudo systemctl start horusdemod.service
```

#### recordings@.service (Template)
Generic recording service template for capturing audio/data.

```bash
sudo systemctl start recordings@vhf.service
sudo systemctl start recordings@uhf.service
```

#### set_lo_multicast.service
Sets up local multicast routing tables. Should typically start at boot.

```bash
sudo systemctl enable set_lo_multicast.service
sudo systemctl start set_lo_multicast.service
```

## Common Operations

### Start a Service
```bash
sudo systemctl start <service-name>
```

### Stop a Service
```bash
sudo systemctl stop <service-name>
```

### Enable a Service (Start at Boot)
```bash
sudo systemctl enable <service-name>
```

### Disable a Service
```bash
sudo systemctl disable <service-name>
```

### Check Service Status
```bash
sudo systemctl status <service-name>
```

### View Service Logs
```bash
# Recent logs
sudo journalctl -u <service-name>

# Follow logs in real-time
sudo journalctl -u <service-name> -f

# Logs from the last hour
sudo journalctl -u <service-name> --since "1 hour ago"
```

### Restart a Service
```bash
sudo systemctl restart <service-name>
```

### Reload Service Configuration
After editing a .service file:
```bash
sudo systemctl daemon-reload
sudo systemctl restart <service-name>
```

## Template Services Explained

Services with `@` in the name (e.g., `radiod@.service`) are **template services**. They allow you to run multiple instances of the same program with different configurations.

### Example: Multiple radiod Instances

```bash
# Start radiod for 2m band
sudo systemctl start radiod@2m.service

# Start radiod for 70cm band  
sudo systemctl start radiod@70cm.service

# Start radiod for HF
sudo systemctl start radiod@hf.service

# Each looks for its config at /etc/radio/%i.conf
# where %i is replaced with the instance name
```

### Creating Custom Template Instances

1. Create your config file:
```bash
sudo cp /etc/radio/radiod.conf /etc/radio/my-custom-config.conf
sudo nano /etc/radio/my-custom-config.conf
```

2. Start the service:
```bash
sudo systemctl start radiod@my-custom-config.service
```

## Configuration File Locations

Most services expect configuration files in `/etc/radio/`:

```
/etc/radio/
├── radiod.conf        # Default radiod config
├── 2m.conf           # 2m band config
├── 70cm.conf         # 70cm band config
├── packetd.conf      # Packet daemon config
└── ...
```

## Troubleshooting

### Service Won't Start

```bash
# Check detailed status
sudo systemctl status <service-name>

# Check logs for errors
sudo journalctl -u <service-name> -n 50

# Check if binary exists
which <program-name>

# Check file permissions
ls -l /usr/local/sbin/<program-name>
ls -l /etc/radio/<config-file>
```

### Service Keeps Restarting

Most services are configured with `Restart=on-failure`, which means they'll restart if they crash. Check logs to see why:

```bash
sudo journalctl -u <service-name> -f
```

### Multicast Not Working

Make sure the multicast setup service is running:

```bash
sudo systemctl status set_lo_multicast.service
sudo systemctl start set_lo_multicast.service
```

### Permission Issues

Services typically run as specific users (often `radio` or `root`). Check the service file:

```bash
grep User /etc/systemd/system/<service-name>.service
```

## Platform Support

### Linux
✅ **Fully Supported** - All systemd services work as described above.

### macOS  
❌ **Not Applicable** - macOS uses launchd instead of systemd.

Future versions may include launchd `.plist` files for macOS.

### FreeBSD
❌ **Not Applicable** - FreeBSD uses rc.d instead of systemd.

You can manually create rc.d scripts in `/usr/local/etc/rc.d/` if needed.

## Security Notes

1. **Run as Non-Root**: Most services should run as a dedicated user (e.g., `radio`):
   ```bash
   sudo useradd -r -s /bin/false radio
   ```

2. **File Permissions**: Ensure config files are readable by the service user:
   ```bash
   sudo chown root:radio /etc/radio/*.conf
   sudo chmod 640 /etc/radio/*.conf
   ```

3. **Hardware Access**: For SDR hardware access, add the service user to appropriate groups:
   ```bash
   sudo usermod -a -G plugdev,dialout radio
   ```

## Customizing Service Files

To modify a service without editing the installed file:

```bash
# Create override directory
sudo systemctl edit <service-name>

# This opens an editor for drop-in overrides
# Add your customizations, for example:
[Service]
Environment="CUSTOM_VAR=value"
ExecStart=
ExecStart=/usr/local/sbin/radiod -v -c /my/custom/config.conf

# Save and reload
sudo systemctl daemon-reload
sudo systemctl restart <service-name>
```

## Contributing

When adding new service files:

1. Place them in the `service/` directory
2. Use descriptive names
3. Include comments explaining the service purpose
4. Test thoroughly before committing
5. Update this documentation

## References

- [systemd documentation](https://www.freedesktop.org/wiki/Software/systemd/)
- [systemd service unit files](https://www.freedesktop.org/software/systemd/man/systemd.service.html)
- [journalctl documentation](https://www.freedesktop.org/software/systemd/man/journalctl.html)
