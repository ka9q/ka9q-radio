#!/bin/bash
# Dependency Analysis Script for ka9q-radio
# This script helps discover direct and transitive dependencies across different OSes

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUTPUT_FILE="${1:-dependency_report.md}"

# Color codes for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo_header() {
    echo -e "${BLUE}=== $1 ===${NC}"
}

echo_success() {
    echo -e "${GREEN}✓ $1${NC}"
}

echo_warning() {
    echo -e "${YELLOW}⚠ $1${NC}"
}

echo_error() {
    echo -e "${RED}✗ $1${NC}"
}

# Detect OS
detect_os() {
    if [[ "$OSTYPE" == "linux-gnu"* ]]; then
        echo "linux"
    elif [[ "$OSTYPE" == "darwin"* ]]; then
        echo "macos"
    elif [[ "$OSTYPE" == "freebsd"* ]]; then
        echo "freebsd"
    else
        echo "unknown"
    fi
}

OS=$(detect_os)

# Initialize report
cat > "$OUTPUT_FILE" << 'HEADER'
# ka9q-radio Dependency Analysis Report

**Generated:** $(date)
**Platform:** $(uname -a)

## Executive Summary

This report analyzes the complete dependency tree for ka9q-radio,
including both direct and transitive (secondary) dependencies.

---

HEADER

# Function to analyze dependencies on Debian/Ubuntu
analyze_debian() {
    echo_header "Analyzing Debian/Ubuntu Dependencies"
    
    cat >> "$OUTPUT_FILE" << 'EOF'
## Debian/Ubuntu Dependencies

### Core Libraries

EOF

    # List of ka9q-radio dependencies from apt
    PACKAGES=(
        "libfftw3-dev"
        "libopus-dev"
        "libncurses5-dev"
        "libbsd-dev"
        "libusb-1.0-0-dev"
        "libairspy-dev"
        "libairspyhf-dev"
        "librtlsdr-dev"
        "libhackrf-dev"
        "libiniparser-dev"
        "libavahi-client-dev"
        "portaudio19-dev"
    )
    
    for pkg in "${PACKAGES[@]}"; do
        echo "#### $pkg" >> "$OUTPUT_FILE"
        echo "" >> "$OUTPUT_FILE"
        
        if dpkg -l | grep -q "^ii.*$pkg"; then
            echo_success "$pkg is installed"
            echo "**Status:** Installed" >> "$OUTPUT_FILE"
            echo "" >> "$OUTPUT_FILE"
            
            # Get dependencies
            echo "**Direct Dependencies:**" >> "$OUTPUT_FILE"
            echo '```' >> "$OUTPUT_FILE"
            apt-cache depends "$pkg" 2>/dev/null | grep "Depends:" | sed 's/.*Depends: /  - /' >> "$OUTPUT_FILE" || echo "  (none found)" >> "$OUTPUT_FILE"
            echo '```' >> "$OUTPUT_FILE"
            echo "" >> "$OUTPUT_FILE"
            
            # Get reverse dependencies
            echo "**Reverse Dependencies (what needs this):**" >> "$OUTPUT_FILE"
            echo '```' >> "$OUTPUT_FILE"
            apt-cache rdepends "$pkg" 2>/dev/null | head -20 | tail -n +3 | sed 's/^/  - /' >> "$OUTPUT_FILE" || echo "  (none found)" >> "$OUTPUT_FILE"
            echo '```' >> "$OUTPUT_FILE"
            echo "" >> "$OUTPUT_FILE"
        else
            echo_warning "$pkg is not installed"
            echo "**Status:** Not installed" >> "$OUTPUT_FILE"
            echo "" >> "$OUTPUT_FILE"
        fi
    done
    
    # pkg-config analysis
    echo "" >> "$OUTPUT_FILE"
    echo "### pkg-config Analysis" >> "$OUTPUT_FILE"
    echo "" >> "$OUTPUT_FILE"
    
    PKG_CONFIGS=(
        "fftw3f"
        "opus"
        "ncurses"
        "libbsd"
        "libusb-1.0"
    )
    
    for pc in "${PKG_CONFIGS[@]}"; do
        if pkg-config --exists "$pc" 2>/dev/null; then
            echo_success "Found pkg-config: $pc"
            echo "#### $pc" >> "$OUTPUT_FILE"
            echo '```' >> "$OUTPUT_FILE"
            echo "Requires:" >> "$OUTPUT_FILE"
            pkg-config --print-requires "$pc" 2>/dev/null | sed 's/^/  /' || echo "  (none)" >> "$OUTPUT_FILE"
            echo "" >> "$OUTPUT_FILE"
            echo "Requires.private:" >> "$OUTPUT_FILE"
            pkg-config --print-requires-private "$pc" 2>/dev/null | sed 's/^/  /' || echo "  (none)" >> "$OUTPUT_FILE"
            echo "" >> "$OUTPUT_FILE"
            echo "Libs: $(pkg-config --libs $pc 2>/dev/null)" >> "$OUTPUT_FILE"
            echo "Cflags: $(pkg-config --cflags $pc 2>/dev/null)" >> "$OUTPUT_FILE"
            echo '```' >> "$OUTPUT_FILE"
            echo "" >> "$OUTPUT_FILE"
        fi
    done
}

# Function to analyze dependencies on MacOS
analyze_macos() {
    echo_header "Analyzing MacOS (Homebrew) Dependencies"
    
    cat >> "$OUTPUT_FILE" << 'EOF'
## MacOS (Homebrew) Dependencies

### Core Libraries

EOF

    if ! command -v brew &> /dev/null; then
        echo_error "Homebrew not found. Install from https://brew.sh"
        echo "**Error:** Homebrew not installed" >> "$OUTPUT_FILE"
        return
    fi
    
    FORMULAS=(
        "fftw"
        "opus"
        "ncurses"
        "libbsd"
        "libusb"
        "hidapi"
        "airspy"
        "hackrf"
        "iniparser"
        "avahi"
        "portaudio"
        "rtl-sdr"
    )
    
    for formula in "${FORMULAS[@]}"; do
        echo "#### $formula" >> "$OUTPUT_FILE"
        echo "" >> "$OUTPUT_FILE"
        
        if brew list "$formula" &> /dev/null; then
            echo_success "$formula is installed"
            echo "**Status:** Installed" >> "$OUTPUT_FILE"
            echo "" >> "$OUTPUT_FILE"
            
            # Get direct dependencies
            echo "**Direct Dependencies:**" >> "$OUTPUT_FILE"
            echo '```' >> "$OUTPUT_FILE"
            brew deps "$formula" 2>/dev/null | sed 's/^/  - /' || echo "  (none)" >> "$OUTPUT_FILE"
            echo '```' >> "$OUTPUT_FILE"
            echo "" >> "$OUTPUT_FILE"
            
            # Get dependency tree (limited depth)
            echo "**Dependency Tree:**" >> "$OUTPUT_FILE"
            echo '```' >> "$OUTPUT_FILE"
            brew deps --tree "$formula" 2>/dev/null | head -30 || echo "  (error getting tree)" >> "$OUTPUT_FILE"
            echo '```' >> "$OUTPUT_FILE"
            echo "" >> "$OUTPUT_FILE"
            
            # Get reverse dependencies
            echo "**Used By:**" >> "$OUTPUT_FILE"
            echo '```' >> "$OUTPUT_FILE"
            brew uses --installed "$formula" 2>/dev/null | sed 's/^/  - /' || echo "  (none)" >> "$OUTPUT_FILE"
            echo '```' >> "$OUTPUT_FILE"
            echo "" >> "$OUTPUT_FILE"
        else
            echo_warning "$formula is not installed"
            echo "**Status:** Not installed (but available)" >> "$OUTPUT_FILE"
            echo "" >> "$OUTPUT_FILE"
            
            # Show what it would need
            echo "**Would Depend On:**" >> "$OUTPUT_FILE"
            echo '```' >> "$OUTPUT_FILE"
            brew deps "$formula" 2>/dev/null | sed 's/^/  - /' || echo "  (not available)" >> "$OUTPUT_FILE"
            echo '```' >> "$OUTPUT_FILE"
            echo "" >> "$OUTPUT_FILE"
        fi
    done
    
    # Highlight MacOS-specific differences
    cat >> "$OUTPUT_FILE" << 'EOF'

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

EOF
}

# Function to analyze dependencies on FreeBSD
analyze_freebsd() {
    echo_header "Analyzing FreeBSD Dependencies"
    
    cat >> "$OUTPUT_FILE" << 'EOF'
## FreeBSD Dependencies

### Core Libraries

EOF

    if ! command -v pkg &> /dev/null; then
        echo_error "pkg not found"
        echo "**Error:** pkg not available" >> "$OUTPUT_FILE"
        return
    fi
    
    PACKAGES=(
        "fftw3"
        "opus"
        "ncurses"
        "libbsd"
        "libusb"
        "libhidapi"
    )
    
    for pkg_name in "${PACKAGES[@]}"; do
        echo "#### $pkg_name" >> "$OUTPUT_FILE"
        echo "" >> "$OUTPUT_FILE"
        
        if pkg info "$pkg_name" &> /dev/null; then
            echo_success "$pkg_name is installed"
            echo "**Status:** Installed" >> "$OUTPUT_FILE"
            echo "" >> "$OUTPUT_FILE"
            
            echo "**Dependencies:**" >> "$OUTPUT_FILE"
            echo '```' >> "$OUTPUT_FILE"
            pkg info -d "$pkg_name" | tail -n +2 | sed 's/^/  - /' >> "$OUTPUT_FILE"
            echo '```' >> "$OUTPUT_FILE"
            echo "" >> "$OUTPUT_FILE"
            
            echo "**Required By:**" >> "$OUTPUT_FILE"
            echo '```' >> "$OUTPUT_FILE"
            pkg info -r "$pkg_name" | tail -n +2 | sed 's/^/  - /' >> "$OUTPUT_FILE"
            echo '```' >> "$OUTPUT_FILE"
            echo "" >> "$OUTPUT_FILE"
        else
            echo_warning "$pkg_name is not installed"
            echo "**Status:** Not installed" >> "$OUTPUT_FILE"
            echo "" >> "$OUTPUT_FILE"
        fi
    done
}

# Analyze system libraries and their linkage
analyze_system_libs() {
    echo_header "Analyzing System Library Linkage"
    
    cat >> "$OUTPUT_FILE" << 'EOF'

## System Library Analysis

This section shows actual runtime library dependencies by examining
installed libraries.

EOF

    # Find common library locations
    case "$OS" in
        linux)
            LIB_PATHS=(/usr/lib /usr/local/lib /lib)
            ;;
        macos)
            LIB_PATHS=(/usr/local/lib /opt/homebrew/lib /usr/lib)
            ;;
        freebsd)
            LIB_PATHS=(/usr/local/lib /usr/lib /lib)
            ;;
    esac
    
    for lib_path in "${LIB_PATHS[@]}"; do
        if [[ -d "$lib_path" ]]; then
            echo "### Libraries in $lib_path" >> "$OUTPUT_FILE"
            echo '```' >> "$OUTPUT_FILE"
            
            # Find relevant libraries
            find "$lib_path" -name "libusb*.so*" -o -name "libusb*.dylib" -o -name "libhidapi*" 2>/dev/null | while read -r lib; do
                echo "File: $lib" >> "$OUTPUT_FILE"
                
                case "$OS" in
                    linux)
                        ldd "$lib" 2>/dev/null | grep -E "(libusb|hidapi|pthread)" | sed 's/^/  /' >> "$OUTPUT_FILE" || true
                        ;;
                    macos)
                        otool -L "$lib" 2>/dev/null | grep -E "(libusb|hidapi|pthread)" | sed 's/^/  /' >> "$OUTPUT_FILE" || true
                        ;;
                esac
                echo "" >> "$OUTPUT_FILE"
            done
            
            echo '```' >> "$OUTPUT_FILE"
            echo "" >> "$OUTPUT_FILE"
        fi
    done
}

# Create comparison matrix
create_comparison_matrix() {
    cat >> "$OUTPUT_FILE" << 'EOF'

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

EOF
}

# Create recommendations section
create_recommendations() {
    cat >> "$OUTPUT_FILE" << 'EOF'

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

EOF
}

# Main execution
main() {
    echo_header "Starting Dependency Analysis"
    echo ""
    
    case "$OS" in
        linux)
            analyze_debian
            ;;
        macos)
            analyze_macos
            ;;
        freebsd)
            analyze_freebsd
            ;;
        *)
            echo_error "Unsupported OS: $OS"
            ;;
    esac
    
    analyze_system_libs
    create_comparison_matrix
    create_recommendations
    
    echo ""
    echo_header "Analysis Complete"
    echo_success "Report written to: $OUTPUT_FILE"
    echo ""
    echo "To view the report:"
    echo "  cat $OUTPUT_FILE"
    echo ""
    echo "Or convert to HTML with:"
    echo "  pandoc $OUTPUT_FILE -o dependency_report.html"
}

main
