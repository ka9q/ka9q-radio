# CMake Quick Reference

## Basic Build Commands

```bash
# Standard build (portable)
cmake -B build
cmake --build build -j$(nproc)

# Native optimized build (fastest)
cmake -B build -DENABLE_NATIVE_ARCH=ON
cmake --build build -j$(nproc)

# Debug build
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)

# Using presets (recommended)
cmake --preset=default
cmake --build build -j$(nproc)
```

## Available Presets

```bash
cmake --preset=default          # Standard portable build
cmake --preset=release-native   # Native optimization
cmake --preset=debug           # Debug symbols
cmake --preset=minimal         # Core only, no drivers
cmake --preset=rpi             # Raspberry Pi optimized
```

## Build Options

| Option | Default | Description |
|--------|---------|-------------|
| `CMAKE_BUILD_TYPE` | Release | Debug/Release/RelWithDebInfo/MinSizeRel |
| `ENABLE_NATIVE_ARCH` | OFF | Enable -march=native (not portable) |
| `ENABLE_SDR_DRIVERS` | ON | Build SDR driver modules |
| `ENABLE_AVAHI` | ON | Avahi service discovery |
| `ENABLE_XATTR` | ON | Extended attributes support |

## Example Configurations

### Development Build
```bash
cmake --preset=debug
cmake --build build-debug
```

### Production Build (Portable)
```bash
cmake --preset=default
cmake --build build -j$(nproc)
sudo cmake --install build
```

### Maximum Performance (Single Machine)
```bash
cmake --preset=release-native
cmake --build build-release-native -j$(nproc)
```

### Minimal Build (Embedded)
```bash
cmake --preset=minimal
cmake --build build-minimal -j$(nproc)
```

## Optimization Levels by Build Type

| Build Type | Flags | Use Case |
|------------|-------|----------|
| Release | -O3 | Production |
| Debug | -O0 -g | Development |
| RelWithDebInfo | -O2 -g | Profiling |
| MinSizeRel | -Os | Embedded |

## Common Tasks

### Clean rebuild
```bash
rm -rf build
cmake -B build
cmake --build build -j$(nproc)
```

### Install
```bash
sudo cmake --install build
# Installs to /usr/local by default
```

### Custom install prefix
```bash
cmake -B build -DCMAKE_INSTALL_PREFIX=/opt/ka9q-radio
cmake --build build -j$(nproc)
sudo cmake --install build
```

### List configuration
```bash
cmake -L build
# or
cmake -LAH build  # Advanced, with help text
```

### Verbose build
```bash
cmake --build build --verbose
```

## Troubleshooting

### Missing dependencies
```bash
# Debian/Ubuntu
sudo apt install libfftw3-dev libopus-dev libiniparser-dev \
                 libhidapi-dev libusb-1.0-0-dev

# macOS
brew install fftw opus iniparser hidapi libusb

# FreeBSD  
pkg install fftw3 opus iniparser libusb hidapi
```

### Clear CMake cache
```bash
rm -rf build/CMakeCache.txt build/CMakeFiles
cmake -B build
```

### Check what's found
```bash
cmake -B build 2>&1 | grep "Found\|not found"
```

## File Locations After Install

| Item | Location |
|------|----------|
| radiod | /usr/local/sbin/radiod |
| Tools | /usr/local/bin/* |
| Drivers | /usr/local/lib/ka9q-radio/*.so |
| Config | /etc/radio/ |

## Build Artifacts

| File | Location | Description |
|------|----------|-------------|
| radiod | build/src/radiod | Main daemon |
| Drivers | build/src/*.so | SDR driver modules |
| Tools | build/src/control, etc. | Utility programs |
| Config | build/generated/ | Generated headers |

## Preset Matrix

| Preset | Build Type | Native | Drivers | Size |
|--------|-----------|--------|---------|------|
| default | Release | No | Yes | Medium |
| release-native | Release | Yes | Yes | Medium |
| debug | Debug | No | Yes | Large |
| minimal | Release | No | No | Small |
| rpi | Release | No | Yes | Medium |

## CMake Version Requirements

- **Minimum:** 3.16
- **Recommended:** 3.20+
- **Presets require:** 3.21+

Check version: `cmake --version`

## Performance Comparison

### Portable (default)
- ✅ Works everywhere
- ✅ Distributable
- ⚠️  10-30% slower

### Native (opt-in)
- ✅ Maximum speed
- ❌ CPU-specific
- ❌ May crash on different CPUs

## Quick Checks

```bash
# Check if radiod built
test -f build/src/radiod && echo "OK" || echo "FAIL"

# Check drivers
ls build/src/*.so 2>/dev/null | wc -l

# Check tools
ls build/src/control build/src/monitor 2>/dev/null

# Run help
build/src/radiod --help
```

## Environment Variables

```bash
# Custom pkg-config path
export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:$PKG_CONFIG_PATH

# Parallel jobs
export CMAKE_BUILD_PARALLEL_LEVEL=8

# Verbose output
export VERBOSE=1
```

## IDE Integration

### VSCode
```json
// .vscode/settings.json
{
  "cmake.configureOnOpen": true,
  "C_Cpp.default.compileCommands": "${workspaceFolder}/build/compile_commands.json"
}
```

### CLion
Works out of the box with CMakeLists.txt

### Vim/Neovim
Uses build/compile_commands.json automatically with clangd

## Cross-Compilation Example

```bash
# ARM64 cross-compile
cmake -B build-arm64 \
  -DCMAKE_SYSTEM_NAME=Linux \
  -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
  -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc \
  -DENABLE_NATIVE_ARCH=OFF

cmake --build build-arm64
```

## Summary

**Default behavior:** Portable, optimized, includes all drivers  
**For max speed:** Add `-DENABLE_NATIVE_ARCH=ON`  
**For debugging:** Use `--preset=debug`  
**For distribution:** Use default (portable)

---

**Most Common Command:**
```bash
cmake --preset=default && cmake --build build -j$(nproc)
```
