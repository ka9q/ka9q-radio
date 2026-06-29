# Discovering Secondary Dependencies Per OS

## Overview of Tools and Techniques

### 1. Package Manager Dependency Queries

#### **Debian/Ubuntu (apt/dpkg)**
```bash
# Show dependencies of a package
apt-cache depends libusb-1.0-0-dev

# Show reverse dependencies (what depends on this)
apt-cache rdepends libusb-1.0-0-dev

# Show detailed dependency tree
apt-rdepends libusb-1.0-0-dev

# Show installed package dependencies
dpkg -I /path/to/package.deb
```

#### **MacOS (Homebrew)**
```bash
# Show direct dependencies
brew deps libusb

# Show full dependency tree
brew deps --tree libusb

# Show dependencies with annotations
brew deps --annotate libusb

# Show installed dependencies
brew deps --installed libusb

# Show dependencies including build-time deps
brew deps --include-build libusb
```

#### **FreeBSD (pkg)**
```bash
# Show dependencies
pkg info -d libusb

# Show full dependency tree
pkg query -e '%n' '%dn-%dv' libusb

# Show reverse dependencies
pkg info -r libusb

# List all dependencies recursively
pkg info -dx libusb
```

### 2. Build System Tools

#### **pkg-config**
```bash
# List all available pkg-config packages
pkg-config --list-all

# Show dependencies for a library
pkg-config --print-requires libusb-1.0

# Show all dependencies (including private/indirect)
pkg-config --print-requires-private libusb-1.0

# Get compilation flags (shows what it links against)
pkg-config --libs libusb-1.0
pkg-config --cflags libusb-1.0
```

#### **CMake Introspection**
```cmake
# In CMakeLists.txt
find_package(PkgConfig)
pkg_check_modules(LIBUSB libusb-1.0)

# These variables will contain dependency info:
# ${LIBUSB_LIBRARIES}
# ${LIBUSB_INCLUDE_DIRS}
# ${LIBUSB_LDFLAGS}
```

### 3. Binary Analysis Tools

#### **ldd (Linux) / otool (MacOS)**
```bash
# Linux: Show shared library dependencies
ldd /path/to/binary

# MacOS: Show shared library dependencies
otool -L /path/to/binary

# FreeBSD: Show shared library dependencies
ldd /path/to/binary
```

#### **readelf (Linux)**
```bash
# Show dynamic section (including needed libraries)
readelf -d /path/to/binary

# Show all dependencies
readelf -a /path/to/binary | grep NEEDED
```

### 4. Source Code Analysis

#### **grep for includes**
```bash
# Find all #include directives
grep -r "#include" . | grep -E "<.*\.h>"

# Find pkg-config calls in build files
grep -r "pkg-config\|PKG_CHECK_MODULES" .

# Find library links in Makefiles
grep -r "\-l[a-z]" Makefile*
```

