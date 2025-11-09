# ka9q-radio Dependency Analysis Report

**Generated:** $(date)
**Platform:** $(uname -a)

## Executive Summary

This report analyzes the complete dependency tree for ka9q-radio,
including both direct and transitive (secondary) dependencies.

---

## MacOS (Homebrew) Dependencies

### Core Libraries

#### fftw

**Status:** Installed

**Direct Dependencies:**
```
```

**Dependency Tree:**
```
```

**Used By:**
```
```

#### opus

**Status:** Installed

**Direct Dependencies:**
```
```

**Dependency Tree:**
```
```

**Used By:**
```
```

#### ncurses

**Status:** Installed

**Direct Dependencies:**
```
```

**Dependency Tree:**
```
```

**Used By:**
```
```

#### libbsd

**Status:** Installed

**Direct Dependencies:**
```
```

**Dependency Tree:**
```
```

**Used By:**
```
```

#### libusb

**Status:** Installed

**Direct Dependencies:**
```
```

**Dependency Tree:**
```
```

**Used By:**
```
```

#### hidapi

**Status:** Not installed (but available)

**Would Depend On:**
```
```

#### airspy

**Status:** Installed

**Direct Dependencies:**
```
```

**Dependency Tree:**
```
```

**Used By:**
```
```

#### hackrf

**Status:** Installed

**Direct Dependencies:**
```
```

**Dependency Tree:**
```
```

**Used By:**
```
```

#### iniparser

**Status:** Installed

**Direct Dependencies:**
```
```

**Dependency Tree:**
```
```

**Used By:**
```
```

#### avahi

**Status:** Not installed (but available)

**Would Depend On:**
```
```

#### portaudio

**Status:** Installed

**Direct Dependencies:**
```
```

**Dependency Tree:**
```
```

**Used By:**
```
```

#### rtl-sdr

**Status:** Installed

**Direct Dependencies:**
```
```

**Dependency Tree:**
```
```

**Used By:**
```
```


### MacOS-Specific Notes

**HID API on MacOS:**
- MacOS uses IOHIDManager backend (native macOS HID API)
- Unlike Linux, does NOT use libusb backend for HID
- HIDAPI is a separate library: `brew install hidapi`
- libusb should NOT be used for HID devices on MacOS

**System Frameworks:**
MacOS may require additional frameworks:
- IOKit (for USB device access)
- CoreFoundation (for system services)
- SystemConfiguration (for network configuration)

These are typically available by default but may need explicit linking.


## System Library Analysis

This section shows actual runtime library dependencies by examining
installed libraries.

### Libraries in /usr/local/lib
```
```

### Libraries in /opt/homebrew/lib
```
File: /opt/homebrew/lib/libusb.dylib
  /opt/homebrew/lib/libusb.dylib:
  	/opt/homebrew/opt/libusb-compat/lib/libusb-0.1.4.dylib (compatibility version 9.0.0, current version 9.4.0)
  	/opt/homebrew/opt/libusb/lib/libusb-1.0.0.dylib (compatibility version 4.0.0, current version 4.0.0)

File: /opt/homebrew/lib/libusb-1.0.dylib
  /opt/homebrew/lib/libusb-1.0.dylib:
  	/opt/homebrew/opt/libusb/lib/libusb-1.0.0.dylib (compatibility version 6.0.0, current version 6.0.0)

File: /opt/homebrew/lib/libusb-0.1.4.dylib
  /opt/homebrew/lib/libusb-0.1.4.dylib:
  	/opt/homebrew/opt/libusb-compat/lib/libusb-0.1.4.dylib (compatibility version 9.0.0, current version 9.4.0)
  	/opt/homebrew/opt/libusb/lib/libusb-1.0.0.dylib (compatibility version 4.0.0, current version 4.0.0)

File: /opt/homebrew/lib/libusb-1.0.0.dylib
  /opt/homebrew/lib/libusb-1.0.0.dylib:
  	/opt/homebrew/opt/libusb/lib/libusb-1.0.0.dylib (compatibility version 6.0.0, current version 6.0.0)

```

### Libraries in /usr/lib
```
```


## Cross-Platform Dependency Comparison

| Library/Feature | Debian/Ubuntu | MacOS (Homebrew) | FreeBSD | Notes |
|----------------|---------------|------------------|---------|-------|
| FFTW3 | libfftw3-dev | fftw | fftw3 | ✓ Available on all |
| Opus | libopus-dev | opus | opus | ✓ Available on all |
| ncurses | libncurses5-dev | ncurses | ncurses | ✓ Available on all |
| libbsd | libbsd-dev | libbsd | libbsd | ✓ Available on all |
| libusb | libusb-1.0-0-dev | libusb | libusb | ✓ Available on all |
| HIDAPI | libhidapi-dev | hidapi | libhidapi | ⚠ Different backends! |
| Airspy | libairspy-dev | airspy | ? | ⚠ May need manual build |
| RTL-SDR | librtlsdr-dev | rtl-sdr | rtl-sdr | ✓ Available on all |
| HackRF | libhackrf-dev | hackrf | hackrf | ✓ Available on all |
| iniparser | libiniparser-dev | iniparser | ? | ⚠ May need alternative |
| Avahi | libavahi-client-dev | avahi | avahi | ✓ Available on all |
| PortAudio | portaudio19-dev | portaudio | portaudio | ✓ Available on all |
| systemd | systemd | ✗ N/A | ✗ N/A | ⚠ Linux-only! |
| udev | udev | ✗ N/A | devd | ⚠ Different systems |

### Key Differences by Platform

#### HIDAPI Backend Differences
- **Linux:** Can use either hidraw or libusb backend
  - `libhidapi-libusb` - uses libusb-1.0
  - `libhidapi-hidraw` - uses kernel hidraw interface
- **MacOS:** Uses IOHIDManager (native API)
  - Does NOT use libusb for HID
  - Requires separate hidapi library
- **FreeBSD:** Uses libusb backend

#### Service Management
- **Linux:** systemd
- **MacOS:** launchd (need .plist files)
- **FreeBSD:** rc.d (need rc scripts)

#### Device Management
- **Linux:** udev rules
- **MacOS:** IOKit (no rules needed usually)
- **FreeBSD:** devd configuration


## Recommendations for Portability

### 1. Build System Improvements

**Use CMake or Meson with proper dependency detection:**

```cmake
# Example CMake approach
find_package(PkgConfig REQUIRED)

# Core dependencies with fallbacks
pkg_check_modules(FFTW REQUIRED fftw3f)
pkg_check_modules(OPUS REQUIRED opus)
pkg_check_modules(USB REQUIRED libusb-1.0)

# Optional hardware support
pkg_check_modules(AIRSPY libairspy)
if(AIRSPY_FOUND)
    add_definitions(-DHAVE_AIRSPY)
endif()

# Platform-specific dependencies
if(APPLE)
    find_library(IOKIT IOKit)
    find_library(COREFOUNDATION CoreFoundation)
    target_link_libraries(radiod ${IOKIT} ${COREFOUNDATION})
endif()
```

### 2. Handle HIDAPI Platform Differences

```c
// Create abstraction layer
#ifdef __APPLE__
    // Use IOHIDManager directly, not libusb
    #include <hidapi/hidapi.h>
#elif defined(__linux__)
    // Can use either backend
    #include <hidapi/hidapi.h>
#elif defined(__FreeBSD__)
    // Use libusb backend
    #include <hidapi/hidapi.h>
#endif
```

### 3. Abstract Service Management

Create a service management abstraction:
- `service/systemd/` - Linux systemd units
- `service/launchd/` - MacOS launch agents
- `service/rc.d/` - FreeBSD rc scripts

### 4. Configuration File Locations

Use platform-appropriate paths:
```c
#ifdef __APPLE__
    #define CONFIG_DIR "/usr/local/etc/ka9q-radio"
#elif defined(__FreeBSD__)
    #define CONFIG_DIR "/usr/local/etc/ka9q-radio"
#else
    #define CONFIG_DIR "/etc/radio"
#endif
```

### 5. Conditional Compilation for Optional Features

```makefile
# Detect available hardware libraries
HAS_AIRSPY := $(shell pkg-config --exists libairspy && echo yes)
HAS_RTLSDR := $(shell pkg-config --exists librtlsdr && echo yes)
HAS_HACKRF := $(shell pkg-config --exists libhackrf && echo yes)

ifeq ($(HAS_AIRSPY),yes)
    CFLAGS += -DHAVE_AIRSPY
    LDFLAGS += $(shell pkg-config --libs libairspy)
endif
```

### 6. Testing Strategy

- Create CI/CD pipelines for:
  - Ubuntu 22.04, 24.04
  - MacOS (latest 2 versions)
  - FreeBSD 13, 14
- Test with and without optional hardware libraries
- Validate all service management configurations

