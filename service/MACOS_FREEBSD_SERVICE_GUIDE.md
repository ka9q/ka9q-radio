# Adding macOS and FreeBSD Service Support

This guide explains how to add daemon/service management for macOS (launchd) and FreeBSD (rc.d) to complement the existing Linux systemd support.

## Quick Reference

| Platform | Format | Install Location | Management Tool |
|----------|--------|------------------|-----------------|
| Linux | `.service` (systemd) | `/etc/systemd/system/` | `systemctl` |
| macOS | `.plist` (launchd) | `/Library/LaunchDaemons/` | `launchctl` |
| FreeBSD | shell scripts (rc.d) | `/usr/local/etc/rc.d/` | `service` |

## Current Status

✅ **Linux (systemd)**: Fully implemented  
⚠️ **macOS (launchd)**: Template provided, needs `.plist` files  
⚠️ **FreeBSD (rc.d)**: Template provided, needs scripts  

---

## macOS: Adding Launchd Support

### Step 1: Create .plist Files

Create property list files for each daemon in the `service/` directory.

**Example: `service/org.ka9q.radio.radiod.plist`**

```xml
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <!-- Service identifier (reverse domain notation) -->
    <key>Label</key>
    <string>org.ka9q.radio.radiod</string>
    
    <!-- Program to run -->
    <key>ProgramArguments</key>
    <array>
        <string>/usr/local/sbin/radiod</string>
        <string>-c</string>
        <string>/etc/radio/radiod.conf</string>
    </array>
    
    <!-- Start at system boot -->
    <key>RunAtLoad</key>
    <true/>
    
    <!-- Keep alive (restart if crashes) -->
    <key>KeepAlive</key>
    <true/>
    
    <!-- Standard output/error -->
    <key>StandardOutPath</key>
    <string>/var/log/radiod.log</string>
    <key>StandardErrorPath</key>
    <string>/var/log/radiod.error.log</string>
    
    <!-- Working directory -->
    <key>WorkingDirectory</key>
    <string>/var/run/ka9q-radio</string>
    
    <!-- Run as user (optional, default is root) -->
    <key>UserName</key>
    <string>radio</string>
    <key>GroupName</key>
    <string>radio</string>
    
    <!-- Environment variables (optional) -->
    <key>EnvironmentVariables</key>
    <dict>
        <key>PATH</key>
        <string>/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin</string>
    </dict>
</dict>
</plist>
```

### Step 2: Create .plist Files for All Daemons

Based on the systemd services, create corresponding .plist files:

- `org.ka9q.radio.radiod.plist` - Radio daemon
- `org.ka9q.radio.packetd.plist` - Packet daemon
- `org.ka9q.radio.aprsfeed.plist` - APRS feed
- `org.ka9q.radio.aprs.plist` - APRS daemon
- `org.ka9q.radio.cwd.plist` - CW decoder
- `org.ka9q.radio.opusd.plist` - Opus streaming
- `org.ka9q.radio.stereod.plist` - Stereo decoder
- `org.ka9q.radio.rdsd.plist` - RDS decoder

### Step 3: Enable in CMakeLists.txt

In `service/CMakeLists.txt`, find the macOS section and uncomment:

```cmake
elseif(APPLE)
  file(GLOB PLIST_FILES "*.plist")
  
  if(PLIST_FILES)
    install(FILES ${PLIST_FILES}
            DESTINATION /Library/LaunchDaemons
            PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
    
    message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
    message(STATUS "macOS Launchd Property Lists")
    message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
    message(STATUS "  Install location: /Library/LaunchDaemons")
    message(STATUS "")
    message(STATUS "After installation:")
    message(STATUS "  1. Load daemon:   sudo launchctl load /Library/LaunchDaemons/<plist>")
    message(STATUS "  2. Start daemon:  sudo launchctl start <label>")
    message(STATUS "  3. Stop daemon:   sudo launchctl stop <label>")
    message(STATUS "  4. Unload daemon: sudo launchctl unload /Library/LaunchDaemons/<plist>")
    message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
  endif()
```

### Step 4: Test on macOS

```bash
# Build and install
cmake -B build
sudo cmake --install build

# Load the daemon
sudo launchctl load /Library/LaunchDaemons/org.ka9q.radio.radiod.plist

# Start manually (if not RunAtLoad)
sudo launchctl start org.ka9q.radio.radiod

# Check status
sudo launchctl list | grep ka9q

# View logs
tail -f /var/log/radiod.log

# Stop daemon
sudo launchctl stop org.ka9q.radio.radiod

# Unload (removes from startup)
sudo launchctl unload /Library/LaunchDaemons/org.ka9q.radio.radiod.plist
```

### Launchd Resources

- Official: https://www.launchd.info/
- Apple Docs: https://developer.apple.com/library/archive/documentation/MacOSX/Conceptual/BPSystemStartup/
- Examples: https://github.com/topics/launchd

---

## FreeBSD: Adding rc.d Support

### Step 1: Create rc.d Directory

```bash
mkdir -p service/rc.d
```

### Step 2: Create rc.d Scripts

Create executable shell scripts for each daemon in `service/rc.d/`.

**Example: `service/rc.d/radiod`**

```bash
#!/bin/sh
#
# PROVIDE: radiod
# REQUIRE: LOGIN DAEMON networking
# KEYWORD: shutdown
#
# Add these lines to /etc/rc.conf to enable radiod:
#
# radiod_enable="YES"
# radiod_config="/etc/radio/radiod.conf"
# radiod_flags=""  # Additional flags

. /etc/rc.subr

name="radiod"
rcvar="${name}_enable"

# Defaults
load_rc_config "${name}"
: ${radiod_enable:="NO"}
: ${radiod_config:="/etc/radio/radiod.conf"}
: ${radiod_user:="radio"}
: ${radiod_group:="radio"}
: ${radiod_flags:=""}

# Daemon configuration
command="/usr/local/sbin/${name}"
pidfile="/var/run/${name}.pid"
command_args="-c ${radiod_config} ${radiod_flags}"

# Process management
start_precmd="${name}_prestart"
stop_postcmd="${name}_poststop"

radiod_prestart()
{
    # Create PID directory if needed
    if [ ! -d "$(dirname ${pidfile})" ]; then
        install -d -o ${radiod_user} -g ${radiod_group} "$(dirname ${pidfile})"
    fi
    
    # Check if config file exists
    if [ ! -f "${radiod_config}" ]; then
        err 1 "Config file not found: ${radiod_config}"
    fi
}

radiod_poststop()
{
    # Clean up PID file
    rm -f "${pidfile}"
}

run_rc_command "$1"
```

Make it executable:
```bash
chmod +x service/rc.d/radiod
```

### Step 3: Create Scripts for All Daemons

Based on systemd services, create scripts:

- `service/rc.d/radiod` - Radio daemon
- `service/rc.d/packetd` - Packet daemon
- `service/rc.d/aprsfeed` - APRS feed
- `service/rc.d/aprs` - APRS daemon
- `service/rc.d/cwd` - CW decoder
- `service/rc.d/opusd` - Opus streaming
- `service/rc.d/stereod` - Stereo decoder
- `service/rc.d/rdsd` - RDS decoder

### Step 4: Enable in CMakeLists.txt

In `service/CMakeLists.txt`, find the FreeBSD section and uncomment:

```cmake
elseif(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
  file(GLOB RCD_SCRIPTS "rc.d/*")
  
  if(RCD_SCRIPTS)
    install(PROGRAMS ${RCD_SCRIPTS}
            DESTINATION /usr/local/etc/rc.d
            PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                       GROUP_READ GROUP_EXECUTE
                       WORLD_READ WORLD_EXECUTE)
    
    message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
    message(STATUS "FreeBSD rc.d Scripts")
    message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
    message(STATUS "  Install location: /usr/local/etc/rc.d")
    message(STATUS "")
    message(STATUS "After installation:")
    message(STATUS "  1. Enable service: echo 'radiod_enable=\"YES\"' >> /etc/rc.conf")
    message(STATUS "  2. Start service:  sudo service radiod start")
    message(STATUS "  3. Stop service:   sudo service radiod stop")
    message(STATUS "  4. Check status:   sudo service radiod status")
    message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
  endif()
```

### Step 5: Test on FreeBSD

```bash
# Build and install
cmake -B build
sudo cmake --install build

# Enable the service
sudo sysrc radiod_enable="YES"

# Optional: Set custom config
sudo sysrc radiod_config="/etc/radio/my-config.conf"

# Start the service
sudo service radiod start

# Check status
sudo service radiod status

# View logs
tail -f /var/log/messages | grep radiod

# Stop service
sudo service radiod stop

# Disable service
sudo sysrc radiod_enable="NO"
```

### rc.d Resources

- Official: https://docs.freebsd.org/en/articles/rc-scripting/
- Handbook: https://docs.freebsd.org/en/books/handbook/config/#configtuning-rcd
- Examples: /usr/local/etc/rc.d/ on any FreeBSD system

---

## Testing Your Implementation

### Checklist

- [ ] Created service definitions (.plist or rc.d scripts)
- [ ] Made scripts executable (FreeBSD only)
- [ ] Uncommented CMakeLists.txt section
- [ ] Tested build: `cmake -B build`
- [ ] Tested install: `sudo cmake --install build`
- [ ] Verified files installed to correct location
- [ ] Started service successfully
- [ ] Checked logs for errors
- [ ] Verified service restarts after crash (if configured)
- [ ] Tested service stops cleanly
- [ ] Documented any platform-specific quirks

### Common Issues

**macOS:**
- Permissions: .plist files must be owned by root:wheel
- Label must be unique across all launchd jobs
- StandardOutPath/ErrorPath directories must exist

**FreeBSD:**
- Scripts must be executable (755 permissions)
- Must source /etc/rc.subr
- Variables must use ${name}_variable convention
- rcvar must be set

---

## Converting from systemd to launchd/rc.d

### systemd → launchd Mapping

| systemd | launchd |
|---------|---------|
| `ExecStart=` | `ProgramArguments` array |
| `User=` | `UserName` |
| `Group=` | `GroupName` |
| `WorkingDirectory=` | `WorkingDirectory` |
| `Restart=always` | `KeepAlive` = true |
| `Environment=` | `EnvironmentVariables` dict |
| `StandardOutput=` | `StandardOutPath` |
| `StandardError=` | `StandardErrorPath` |

### systemd → rc.d Mapping

| systemd | rc.d |
|---------|------|
| `ExecStart=` | `command=` and `command_args=` |
| `User=` | `${name}_user=` |
| `WorkingDirectory=` | Set in `${name}_prestart` |
| `Restart=always` | Built-in via rc.subr |
| `Environment=` | Set in script or prestart |
| `PIDFile=` | `pidfile=` |

---

## Directory Structure

After implementation, your service directory should look like:

```
service/
├── CMakeLists.txt                      # Updated with macOS/FreeBSD support
├── *.service                           # Linux systemd (existing)
├── org.ka9q.radio.*.plist             # macOS launchd (new)
└── rc.d/                               # FreeBSD rc.d (new)
    ├── radiod
    ├── packetd
    ├── aprsfeed
    └── ...
```

---

## Next Steps

1. **Choose a platform** to implement first (macOS or FreeBSD)
2. **Create one service** as a proof of concept
3. **Test thoroughly** on that platform
4. **Document any issues** or platform-specific requirements
5. **Create remaining services** once template is validated
6. **Update CMakeLists.txt** to enable installation
7. **Test the full build/install/run cycle**
8. **Contribute back** to the project!

Good luck! The CMakeLists.txt file has clear hooks and templates to guide you through the implementation.
