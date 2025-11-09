# Practical Guide: Discovering Secondary Dependencies

## Real Example: libusb and hidapi on Different Platforms

### The Problem

You mentioned that on MacOS, libusb has a dependency on hidapi. Let's explore
why this differs across platforms and how to discover it.

## Discovery Methods by Platform

### 1. MacOS (using Homebrew)

#### Check if libusb is installed
```bash
brew list libusb
```

#### View direct dependencies
```bash
brew deps libusb
# Output might show: No dependencies
```

#### View the full dependency tree
```bash
brew deps --tree libusb
# Shows hierarchical dependencies
```

#### Check what depends ON libusb
```bash
brew uses --installed libusb
# This shows packages that depend on libusb
# You might see: hidapi, hackrf, rtl-sdr, etc.
```

#### The IMPORTANT distinction on MacOS:

**hidapi does NOT depend on libusb on MacOS!**

Run this to verify:
```bash
brew deps hidapi
# Output: No dependencies (or just system frameworks)

brew info hidapi
# Shows: Uses IOHIDManager (native macOS API)
```

Instead, on MacOS:
- **hidapi** uses the native IOHIDManager framework (part of IOKit)
- **libusb** is for direct USB device access
- They are SEPARATE and used for different purposes

### 2. Linux (Debian/Ubuntu)

#### Check package dependencies
```bash
apt-cache depends libusb-1.0-0-dev
# Shows: libc6-dev, libusb-1.0-0, etc.

apt-cache depends libhidapi-dev
# Shows: libhidapi-hidraw0, libhidapi-libusb0
```

#### The key difference on Linux:

```bash
apt-cache show libhidapi-libusb0
# Description: This package contains the libusb-based implementation

apt-cache show libhidapi-hidraw0  
# Description: This package contains the hidraw-based implementation
```

**Linux has TWO hidapi backends:**
1. `libhidapi-libusb` - uses libusb-1.0 (USB only)
2. `libhidapi-hidraw` - uses kernel hidraw (USB + Bluetooth)

Verify the linkage:
```bash
# Find where hidapi libraries are installed
dpkg -L libhidapi-libusb0 | grep "\.so"

# Check what it links against
ldd /usr/lib/x86_64-linux-gnu/libhidapi-libusb.so.0
# Output will show: libusb-1.0.so.0 => /lib/x86_64-linux-gnu/libusb-1.0.so.0
```

### 3. FreeBSD

```bash
# Check package info
pkg info -d libusb

# Check hidapi
pkg info -d libhidapi
# Likely shows: libusb dependency

# Full recursive dependencies
pkg info -dx libhidapi
```

## Using pkg-config to Discover Dependencies

### What is pkg-config?

pkg-config is a helper tool that provides information about installed libraries.
Every library installs a `.pc` file that describes its dependencies.

### Finding .pc files

```bash
# Linux
find /usr/lib /usr/share -name "*.pc" | grep -E "(usb|hid)"

# MacOS (Homebrew)
find /usr/local/lib /opt/homebrew/lib -name "*.pc" | grep -E "(usb|hid)"
```

### Reading .pc files directly

```bash
# Linux example
cat /usr/lib/x86_64-linux-gnu/pkgconfig/libusb-1.0.pc
```

Output might look like:
```
prefix=/usr
libdir=${prefix}/lib/x86_64-linux-gnu
includedir=${prefix}/include

Name: libusb-1.0
Description: C API for USB device access
Version: 1.0.26
Libs: -L${libdir} -lusb-1.0
Cflags: -I${includedir}/libusb-1.0
```

For hidapi on Linux:
```bash
cat /usr/lib/x86_64-linux-gnu/pkgconfig/hidapi-libusb.pc
```

Output:
```
Name: hidapi-libusb
Description: C library for accessing USB and Bluetooth HID devices (libusb backend)
Version: 0.11.0
Requires: libusb-1.0 >= 1.0.9    # <-- HERE IS THE DEPENDENCY!
Libs: -L${libdir} -lhidapi-libusb
Cflags: -I${includedir}/hidapi
```

### Using pkg-config programmatically

```bash
# Show direct dependencies
pkg-config --print-requires hidapi-libusb
# Output: libusb-1.0 >= 1.0.9

# Show all dependencies (including private/internal ones)
pkg-config --print-requires-private hidapi-libusb

# Get linking flags (shows what libraries will be linked)
pkg-config --libs hidapi-libusb
# Output: -lhidapi-libusb

# Static linking shows all dependencies
pkg-config --libs --static hidapi-libusb
# Output: -lhidapi-libusb -lusb-1.0 -ludev -pthread
```

## Binary Analysis Methods

### After building software, check runtime dependencies:

#### Linux (using ldd)
```bash
# Build your program first
gcc myprogram.c -o myprogram $(pkg-config --cflags --libs hidapi-libusb)

# Check what it needs at runtime
ldd myprogram
```

Output:
```
libhidapi-libusb.so.0 => /usr/lib/x86_64-linux-gnu/libhidapi-libusb.so.0
libusb-1.0.so.0 => /lib/x86_64-linux-gnu/libusb-1.0.so.0
libc.so.6 => /lib/x86_64-linux-gnu/libc.so.6
```

#### MacOS (using otool)
```bash
# Build your program
clang myprogram.c -o myprogram -lhidapi

# Check dependencies
otool -L myprogram
```

Output:
```
/usr/local/lib/libhidapi.dylib
/System/Library/Frameworks/IOKit.framework/IOKit
/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation
/usr/lib/libSystem.B.dylib
```

Note: No libusb here! MacOS hidapi uses IOKit instead.

### Using readelf (Linux only)
```bash
readelf -d /usr/lib/x86_64-linux-gnu/libhidapi-libusb.so.0 | grep NEEDED
```

Output:
```
0x0000000000000001 (NEEDED)     Shared library: [libusb-1.0.so.0]
0x0000000000000001 (NEEDED)     Shared library: [libc.so.6]
```

## Creating a Dependency Discovery Script

Here's a simple script to check hidapi dependencies on any system:

```bash
#!/bin/bash

echo "=== Checking hidapi dependencies ==="
echo ""

if command -v pkg-config &> /dev/null; then
    echo "Using pkg-config:"
    
    # Try different pkg-config names
    for name in hidapi hidapi-libusb hidapi-hidraw; do
        if pkg-config --exists $name 2>/dev/null; then
            echo "  Found: $name"
            echo "  Requires: $(pkg-config --print-requires $name)"
            echo "  Libs: $(pkg-config --libs $name)"
            echo ""
        fi
    done
fi

echo "Searching for hidapi libraries:"
if [[ "$OSTYPE" == "darwin"* ]]; then
    # MacOS
    find /usr/local/lib /opt/homebrew/lib -name "*hidapi*" 2>/dev/null | while read f; do
        echo "  Found: $f"
        otool -L "$f" | grep -E "(usb|hid)" | sed 's/^/    /'
    done
elif [[ "$OSTYPE" == "linux-gnu"* ]]; then
    # Linux
    find /usr/lib /lib -name "*hidapi*" 2>/dev/null | while read f; do
        echo "  Found: $f"
        ldd "$f" | grep -E "(usb|hid)" | sed 's/^/    /'
    done
fi
```

## Platform-Specific Dependency Matrices

### For ka9q-radio's USB/HID requirements:

| Component | Linux | MacOS | FreeBSD |
|-----------|-------|-------|---------|
| USB access | libusb-1.0 | libusb | libusb |
| HID (Funcube) | hidapi-libusb OR hidapi-hidraw | hidapi (uses IOKit) | hidapi (uses libusb) |
| Backend | Either hidraw or libusb | IOHIDManager (native) | libusb |
| Additional deps | udev (for hidraw) | IOKit, CoreFoundation | devd |

### Understanding the Architecture:

```
Linux:
  ka9q-radio
    ├─ libhidapi-libusb.so
    │   ├─ libusb-1.0.so
    │   └─ libudev.so
    ├─ libusb-1.0.so
    └─ [other deps]

MacOS:
  ka9q-radio
    ├─ libhidapi.dylib
    │   ├─ IOKit.framework (system)
    │   └─ CoreFoundation.framework (system)
    ├─ libusb.dylib (for non-HID devices)
    └─ [other deps]

FreeBSD:
  ka9q-radio
    ├─ libhidapi.so
    │   └─ libusb.so
    ├─ libusb.so
    └─ [other deps]
```

## Best Practices for Cross-Platform Development

1. **Use pkg-config in build systems:**
   ```cmake
   find_package(PkgConfig REQUIRED)
   pkg_check_modules(HIDAPI REQUIRED hidapi)
   ```

2. **Check for platform-specific backends:**
   ```cmake
   if(LINUX)
       pkg_check_modules(HIDAPI hidapi-hidraw hidapi-libusb)
   elseif(APPLE)
       pkg_check_modules(HIDAPI hidapi)
       find_library(IOKIT IOKit)
   elseif(FreeBSD)
       pkg_check_modules(HIDAPI hidapi)
   endif()
   ```

3. **Document dependencies clearly:**
   - List all dependencies in README
   - Note platform differences
   - Provide installation instructions per platform

4. **Test on all target platforms:**
   - Build on actual hardware or VMs
   - Check runtime dependencies with ldd/otool
   - Verify pkg-config detection works

## Summary

To answer your original question about discovering that libusb has a dependency
on hidapi on MacOS: **it doesn't!** This was a misunderstanding.

The actual relationship is:
- On **Linux**: hidapi-libusb backend depends on libusb
- On **MacOS**: hidapi uses IOKit (native), NOT libusb
- On **FreeBSD**: hidapi depends on libusb

Use these tools to discover the truth:
- `brew deps --tree <package>` (MacOS)
- `apt-cache depends <package>` (Debian/Ubuntu)
- `pkg-config --print-requires <library>`
- `ldd <binary>` (Linux) / `otool -L <binary>` (MacOS)
