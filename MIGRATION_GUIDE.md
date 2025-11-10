# CMake Migration Guide

## Overview

This guide explains how to migrate from your current CMake configuration to the refactored version. The new configuration fixes several bugs and implements CMake best practices while maintaining full compatibility with your build requirements.

## What Changed

### Critical Bug Fixes

1. **Fixed target reference before definition** - The root CMakeLists.txt no longer references the `radio` target before it's created
2. **Fixed hardcoded optimization flags** - Compiler flags now respect `CMAKE_BUILD_TYPE`
3. **Fixed typo in AddDrivers.cmake** - `nclude_guard` → `include_guard`
4. **Fixed case mismatch** - Dependency target names are now consistent (lowercase namespaces)

### Major Improvements

1. **New CompilerFlags.cmake module** - Centralized compiler flag management
2. **Portable builds** - `-march=native` is now optional (disabled by default)
3. **Better dependency handling** - Removed redundant checks and links
4. **CMakePresets.json** - Pre-configured build presets for common scenarios
5. **Cleaner code** - Better comments, organization, and maintainability

## Migration Steps

### Step 1: Backup Your Current Files

```bash
cd /path/to/ka9q-radio
cp CMakeLists.txt CMakeLists.txt.backup
cp -r cmake cmake.backup
cp -r src src.backup
```

### Step 2: Replace Files

Copy the refactored files from the outputs directory:

```bash
# Root CMakeLists.txt
cp /path/to/outputs/CMakeLists.txt .

# cmake/ directory modules
cp /path/to/outputs/cmake/CompilerFlags.cmake cmake/
cp /path/to/outputs/cmake/Options.cmake cmake/
cp /path/to/outputs/cmake/FindDeps.cmake cmake/
cp /path/to/outputs/cmake/DetectSockets.cmake cmake/
cp /path/to/outputs/cmake/DetectAlloca.cmake cmake/
cp /path/to/outputs/cmake/DetectLibusbStringDescriptor.cmake cmake/
cp /path/to/outputs/cmake/AddDrivers.cmake cmake/

# src/CMakeLists.txt
cp /path/to/outputs/src/CMakeLists.txt src/

# Optional: CMake presets
cp /path/to/outputs/CMakePresets.json .
```

### Step 3: Clean Build Directory

```bash
rm -rf build
mkdir build
cd build
```

### Step 4: Configure

#### Option A: Using Presets (Recommended)

```bash
# List available presets
cmake --list-presets

# Configure with a preset
cmake --preset=default
# or
cmake --preset=release-native  # For native optimization
# or
cmake --preset=debug          # For debugging
```

#### Option B: Manual Configuration

```bash
# Standard portable build
cmake ..

# Native optimization (faster but not portable)
cmake -DENABLE_NATIVE_ARCH=ON ..

# Debug build
cmake -DCMAKE_BUILD_TYPE=Debug ..

# Minimal build (no SDR drivers)
cmake -DENABLE_SDR_DRIVERS=OFF ..
```

### Step 5: Build

```bash
cmake --build . -j$(nproc)
```

### Step 6: Verify

```bash
# Check that radiod was built
./src/radiod --help

# Check that drivers were built (if enabled)
ls src/*.so

# Check that tools were built
ls src/control src/monitor
```

## New Build Options

The refactored configuration adds a new option:

| Option | Default | Description |
|--------|---------|-------------|
| `ENABLE_NATIVE_ARCH` | OFF | Enable `-march=native` optimization |

Existing options remain unchanged:
- `ENABLE_AVAHI` (ON) - Avahi service discovery
- `ENABLE_SDR_DRIVERS` (ON) - Build SDR driver modules
- `ENABLE_XATTR` (ON) - Extended attributes support

## Using CMake Presets

The new `CMakePresets.json` provides convenient build configurations:

```bash
# List available presets
cmake --list-presets

# Configure and build in one command
cmake --preset=default && cmake --build build

# Or use specific presets
cmake --preset=release-native  # Optimized for your CPU
cmake --preset=debug          # Debug build
cmake --preset=minimal        # Core only, no drivers
cmake --preset=rpi            # Raspberry Pi optimized
```

## Differences in Behavior

### Compiler Flags

**Before:**
- Always used `-O3 -march=native` regardless of build type
- Flags applied globally to all targets

**After:**
- Respects `CMAKE_BUILD_TYPE`:
  - Release: `-O3`
  - Debug: `-O0 -g`
  - RelWithDebInfo: `-O2 -g`
- `-march=native` only enabled if `ENABLE_NATIVE_ARCH=ON`
- Flags applied via interface library (cleaner)

### Dependency Linking

**Before:**
```cmake
target_link_libraries(radiod
  PRIVATE
    radio
    opus::opus        # Redundant!
    iniparser::iniparser  # Redundant!
)
```

**After:**
```cmake
target_link_libraries(radiod PRIVATE radio m)
# opus and iniparser come transitively from radio
```

This is cleaner and avoids duplicate linking.

## Troubleshooting

### Issue: "Target 'radio' not found"

**Cause:** Old configuration referenced target before definition.

**Solution:** Use the new root CMakeLists.txt which fixes this ordering issue.

### Issue: Build is slower than before

**Cause:** Default build is now portable (no `-march=native`).

**Solution:** Enable native optimization:
```bash
cmake -DENABLE_NATIVE_ARCH=ON ..
# or
cmake --preset=release-native
```

### Issue: Missing HIDAPI/libusb warnings

**Cause:** More informative error messages.

**Solution:** Install required dependencies:
```bash
# Debian/Ubuntu
sudo apt install libhidapi-dev libusb-1.0-0-dev

# macOS
brew install hidapi libusb

# FreeBSD
pkg install libusb hidapi
```

### Issue: Can't find FFTW/Opus/etc.

**Cause:** Dependency detection improved but still requires libraries.

**Solution:** Install missing dependencies:
```bash
# Debian/Ubuntu
sudo apt install libfftw3-dev libopus-dev libiniparser-dev

# macOS
brew install fftw opus iniparser

# FreeBSD
pkg install fftw3 opus iniparser
```

## Reverting to Old Configuration

If you need to revert:

```bash
cp CMakeLists.txt.backup CMakeLists.txt
cp -r cmake.backup/* cmake/
cp -r src.backup/* src/
rm -rf build
```

## Testing the Migration

Run this test sequence to verify everything works:

```bash
# Clean build
rm -rf build && mkdir build && cd build

# Configure
cmake .. -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build . -j$(nproc)

# Test executables exist
test -f src/radiod && echo "✓ radiod built"
test -f src/control && echo "✓ control built"
test -f src/monitor && echo "✓ monitor built"

# Test drivers (if enabled)
ls src/*.so 2>/dev/null && echo "✓ Drivers built"

# Run a quick test
./src/radiod --help
```

## Performance Comparison

You can benchmark the difference between portable and native builds:

```bash
# Build 1: Portable (default)
rm -rf build && mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
time cmake --build . -j$(nproc)

# Build 2: Native optimized
rm -rf build && mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DENABLE_NATIVE_ARCH=ON
time cmake --build . -j$(nproc)
```

Native builds are typically 10-30% faster at runtime but only work on the CPU they were compiled for.

## Next Steps

1. **Test thoroughly** - Build and run your usual workflows
2. **Update CI/CD** - If you have automated builds, update them to use presets
3. **Consider enabling library installation** - Uncomment the installation rules in `src/CMakeLists.txt` if you want to install headers and the static library
4. **Add tests** - Consider adding a `tests/` directory with unit tests

## Getting Help

If you encounter issues with the migration:

1. Check the detailed review document: `cmake_review.md`
2. Compare your build logs before/after migration
3. Enable verbose build output: `cmake --build . --verbose`
4. Check CMake cache: `cmake -L build/`

## Summary of Benefits

✅ Respects CMAKE_BUILD_TYPE  
✅ Portable builds by default  
✅ Fixed target ordering bugs  
✅ Cleaner dependency management  
✅ Better error messages  
✅ IDE/language server support (compile_commands.json)  
✅ Pre-configured build presets  
✅ Easier to maintain and extend  

The refactored configuration maintains full compatibility while following modern CMake best practices. Your existing workflows should work exactly as before, with the added benefit of better portability and maintainability.
