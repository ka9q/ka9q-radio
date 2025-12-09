# CMake Build System for ka9q-radio

This CMakeLists.txt provides a modern, cross-platform build system for ka9q-radio.

## Features

- **Cross-platform support**: Linux, FreeBSD, macOS
- **Automatic dependency detection**: Finds available hardware drivers
- **Package generation**: Creates .deb, .rpm, and FreeBSD .txz packages
- **Flexible configuration**: Enable/disable features via options
- **Parallel with existing Makefiles**: Can coexist with Makefile.linux/Makefile.osx

## Prerequisites

### Debian/Ubuntu
```bash
sudo apt install cmake build-essential \
    libfftw3-dev libopus-dev libbsd-dev libiniparser-dev \
    libavahi-client-dev libusb-1.0-0-dev \
    libairspy-dev libairspyhf-dev librtlsdr-dev libhackrf-dev \
    libncurses-dev portaudio19-dev libasound2-dev \
    libogg-dev libsamplerate0-dev
```

### FreeBSD
```bash
pkg install cmake fftw3-float opus libbsd iniparser \
    libusb rtl-sdr hackrf portaudio ncurses
```

### macOS
```bash
# Using MacPorts
sudo port install cmake fftw-3-single opus libbsd iniparser \
    libusb portaudio ncurses
```

## Building

### Standard Build (Out-of-Source)
```bash
# Clone the repository
git clone https://github.com/ka9q/ka9q-radio.git
cd ka9q-radio

# Create the build directory
mkdir build
cd build

# Configure
cmake ..

# Build (using all CPU cores)
cmake --build . -j$(nproc)

# Install
sudo cmake --install .
```

### Custom Configuration
```bash
# Configure with specific options
cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_HACKRF=OFF \
    -DENABLE_SDRPLAY=ON \
    -DCMAKE_INSTALL_PREFIX=/usr/local

# Or use ccmake for interactive configuration
ccmake ..
```

## Build Options

- `CMAKE_BUILD_TYPE`: `Debug` or `Release` (default: Release)
- `BUILD_SHARED_LIBS`: Build hardware drivers as shared libraries (default: OFF)
- `ENABLE_HACKRF`: Enable HackRF support (default: ON)
- `ENABLE_FOBOS`: Enable Fobos support (default: OFF, requires manual setup)
- `ENABLE_SDRPLAY`: Enable SDRplay support (default: OFF, requires API)
- `ENABLE_BLADERF`: Enable BladeRF support (default: OFF)
- `ENABLE_HYDRASDR`: Enable HydraSDR support (default: OFF)

## Package Generation

### Debian Package
```bash
cd build
cpack -G DEB
# Creates ka9q-radio_1.0.0_amd64.deb
```

### RPM Package
```bash
cd build
cpack -G RPM
# Creates ka9q-radio-1.0.0.x86_64.rpm
```

### FreeBSD Package
```bash
cd build
cpack -G TXZ
# Creates ka9q-radio-1.0.0-FreeBSD.txz
```

## Installation Directories

The CMake build follows standard FHS conventions:

- **Daemons**: `/usr/local/sbin/` (radiod)
- **Programs**: `/usr/local/bin/` (control, monitor, etc.)
- **Support files**: `/usr/local/share/ka9q-radio/`
- **Config files**: `/etc/radio/`
- **State files**: `/var/lib/ka9q-radio/`
- **systemd units**: `/etc/systemd/system/` (Linux only)
- **udev rules**: `/etc/udev/rules.d/` (Linux only)

## Platform-Specific Notes

### Linux with systemd
The build automatically detects systemd and installs service files:
```bash
# Start radiod instance named "hf"
sudo systemctl start radiod@hf

# Enable on boot
sudo systemctl enable radiod@hf
```

### FreeBSD
```bash
# The build can create rc.d scripts for FreeBSD
# (You may need to create freebsd/radiod.in template first)

# Start radiod
sudo service radiod start
```

### macOS
On macOS, only client programs (control, monitor) are typically built,
as radiod is designed for Linux. The CMakeLists.txt will build what it can.

## Transitioning from Makefiles

The CMake build can coexist with existing Makefiles:

```bash
# Old way (still works)
ln -s Makefile.linux Makefile
make -j
sudo make install

# New way (parallel build directory)
mkdir build && cd build
cmake .. && make -j
sudo make install
```

Both install to the same locations by default.

## Troubleshooting

### Missing Dependencies
CMake will report missing dependencies. Check the configuration summary:
```
Features:
  Airspy support: YES
  RTL-SDR support: NO   <-- Install librtlsdr-dev
  HackRF support: YES
```

### Library Not Found
If CMake can't find a library that's installed:
```bash
# Set PKG_CONFIG_PATH
export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:$PKG_CONFIG_PATH
cmake ..
```

### Viewing Configuration
```bash
# See all CMake variables
cmake -L ..

# Or use GUI
cmake-gui ..
```

## Creating Distribution Packages

### For Debian/Ubuntu
1. Build as shown above
2. Create package: `cpack -G DEB`
3. Install: `sudo dpkg -i ka9q-radio_*.deb`

The package handles:
- User/group creation (radio:radio)
- systemd service installation
- udev rules setup
- Configuration file preservation

### For RPM-based Systems
1. Build as shown above
2. Create package: `cpack -G RPM`
3. Install: `sudo rpm -i ka9q-radio-*.rpm`

### For FreeBSD Ports
The CMakeLists.txt can be used as the basis for a FreeBSD port:
1. Create port Makefile that uses CMake
2. Reference this CMakeLists.txt
3. Package with `pkg create`

## Advanced: Adding New Hardware Drivers

To add support for a new SDR device:

1. Add option in CMakeLists.txt:
```cmake
option(ENABLE_MYSDR "Enable MySdr support" OFF)
```

2. Add pkg-config check:
```cmake
if(ENABLE_MYSDR)
    pkg_check_modules(MYSDR libmysdr)
    if(MYSDR_FOUND)
        set(HAVE_MYSDR TRUE)
    endif()
endif()
```

3. Add to driver sources:
```cmake
if(HAVE_MYSDR)
    list(APPEND DRIVER_SOURCES src/mysdr.c)
    list(APPEND DRIVER_LIBS ${MYSDR_LIBRARIES})
endif()
```

## Known Limitations

1. **Source file locations**: This CMakeLists.txt assumes sources are in `src/` 
   and headers in `include/`. Adjust paths if the actual layout differs.

2. **Dynamic library loading**: The recent dynamic driver loading feature 
   needs additional CMake support (not yet implemented).

3. **Platform testing**: Primarily tested on Linux. FreeBSD and macOS support
   is based on documentation review and may need adjustment.

## Contributing

When modifying this CMakeLists.txt:
- Test on multiple platforms if possible
- Update this README with any new options
- Follow CMake best practices
- Keep it compatible with the existing Makefile builds

## References

- [CMake Documentation](https://cmake.org/documentation/)
- [CMake Package Generation](https://cmake.org/cmake/help/latest/module/CPack.html)
- [ka9q-radio INSTALL.md](https://github.com/ka9q/ka9q-radio/blob/main/docs/INSTALL.md)
