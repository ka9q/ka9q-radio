# rules/CMakeLists.txt
# Installation of udev rules for SDR USB devices
#
# These rules control device permissions and symbolic links for SDR hardware,
# allowing non-root users to access USB SDR devices.
#
# Supports:
#   - Linux: udev rules → /etc/udev/rules.d (or /usr/lib/udev/rules.d)
#   - macOS: Not applicable (macOS doesn't use udev)
#   - FreeBSD: devd.conf rules → /usr/local/etc/devd (TODO)

# ==================== LINUX: UDEV RULES ====================
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # Get list of all .rules files in this directory
  file(GLOB UDEV_RULES "*.rules")
  
  # Check if udev is available
  find_program(UDEVADM_EXECUTABLE udevadm)
  
  if(UDEV_RULES)
    # Determine the correct installation directory
    # Modern systems prefer /usr/lib/udev/rules.d, but /etc/udev/rules.d is also valid
    # We'll default to /etc/udev/rules.d for compatibility
    set(UDEV_RULES_DIR "/etc/udev/rules.d" CACHE PATH "Directory for udev rules")
    
    # Alternative: Query udev for the rules directory (requires udevadm)
    if(UDEVADM_EXECUTABLE)
      execute_process(
        COMMAND ${UDEVADM_EXECUTABLE} info --query=property --export --export-prefix=UDEV_ --name=/dev/null
        OUTPUT_VARIABLE _udev_info
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE
      )
      # Could parse UDEV_RULES_DIR from output, but /etc/udev/rules.d is standard
    endif()
    
    # Install udev rules
    install(FILES ${UDEV_RULES}
            DESTINATION ${UDEV_RULES_DIR}
            PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
    
    # Create a custom target for reloading udev rules
    # Note: This won't run automatically during install (requires sudo)
    # Users must run: sudo cmake --build build --target udev-reload
    if(UDEVADM_EXECUTABLE)
      add_custom_target(udev-reload
        COMMAND ${UDEVADM_EXECUTABLE} control --reload-rules
        COMMAND ${UDEVADM_EXECUTABLE} trigger
        COMMENT "Reloading udev rules and triggering device events"
        VERBATIM
      )
    endif()
    
    # Count the rules
    list(LENGTH UDEV_RULES _num_rules)
    
    message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
    message(STATUS "udev Rules for SDR USB Devices")
    message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
    message(STATUS "  Install location: ${UDEV_RULES_DIR}")
    message(STATUS "  Rules count: ${_num_rules}")
    message(STATUS "")
    message(STATUS "These rules allow non-root users to access SDR devices.")
    message(STATUS "")
    message(STATUS "After installation:")
    message(STATUS "  1. Reload udev:    sudo udevadm control --reload-rules")
    message(STATUS "  2. Trigger events: sudo udevadm trigger")
    message(STATUS "  3. Add user to group: sudo usermod -a -G plugdev $USER")
    message(STATUS "  4. Re-login for group changes to take effect")
    message(STATUS "")
    message(STATUS "Or use convenience target:")
    message(STATUS "  sudo cmake --build build --target udev-reload")
    message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
  else()
    message(STATUS "No udev rules files found in rules/ directory")
  endif()

# ==================== MACOS: NOT APPLICABLE ====================
elseif(APPLE)
  # ┌─────────────────────────────────────────────────────────────┐
  # │ macOS Note: USB device permissions                         │
  # │                                                             │
  # │ macOS doesn't use udev. USB devices are typically          │
  # │ accessible to all users by default.                        │
  # │                                                             │
  # │ If you need custom permissions, you can:                   │
  # │ 1. Use IOKit to match devices                              │
  # │ 2. Create custom kext (kernel extension)                   │
  # │ 3. Use system_profiler to inspect USB devices              │
  # │                                                             │
  # │ For most SDR applications, no special configuration is     │
  # │ needed on macOS.                                            │
  # └─────────────────────────────────────────────────────────────┘
  
  message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
  message(STATUS "macOS Detected")
  message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
  message(STATUS "  ℹ️  USB device permissions:")
  message(STATUS "     macOS typically allows all users to access USB devices")
  message(STATUS "     No udev rules needed")
  message(STATUS "")
  message(STATUS "  If you encounter permission issues:")
  message(STATUS "    1. Check System Settings → Privacy & Security")
  message(STATUS "    2. Ensure USB devices are not restricted")
  message(STATUS "    3. Try disconnecting and reconnecting the device")
  message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")

# ==================== FREEBSD: DEVD RULES ====================
elseif(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
  # ┌─────────────────────────────────────────────────────────────┐
  # │ TODO: Add FreeBSD devd support                             │
  # │                                                             │
  # │ FreeBSD uses devd instead of udev for device management.   │
  # │                                                             │
  # │ To implement:                                               │
  # │ 1. Create devd.conf rules in rules/devd/ directory         │
  # │    Example: rules/devd/airspy.conf                         │
  # │                                                             │
  # │ 2. Uncomment and configure the code below                  │
  # │                                                             │
  # │ 3. Test with: sudo cmake --install build                   │
  # │    Then: sudo service devd restart                         │
  # │                                                             │
  # │ devd rule format:                                           │
  # │   attach 100 {                                              │
  # │     match "vendor" "0x1d50";                                │
  # │     match "product" "0x60a1";                               │
  # │     action "chmod 660 /dev/$device-name";                   │
  # │     action "chown root:usb /dev/$device-name";              │
  # │   };                                                        │
  # └─────────────────────────────────────────────────────────────┘
  
  # Uncomment when devd rules are available:
  # file(GLOB DEVD_RULES "devd/*.conf")
  # 
  # if(DEVD_RULES)
  #   install(FILES ${DEVD_RULES}
  #           DESTINATION /usr/local/etc/devd
  #           PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
  #   
  #   # Create target to restart devd
  #   add_custom_target(devd-reload
  #     COMMAND service devd restart
  #     COMMENT "Restarting devd daemon"
  #     VERBATIM
  #   )
  #   
  #   list(LENGTH DEVD_RULES _num_rules)
  #   
  #   message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
  #   message(STATUS "FreeBSD devd Rules for SDR USB Devices")
  #   message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
  #   message(STATUS "  Install location: /usr/local/etc/devd")
  #   message(STATUS "  Rules count: ${_num_rules}")
  #   message(STATUS "")
  #   message(STATUS "After installation:")
  #   message(STATUS "  1. Restart devd: sudo service devd restart")
  #   message(STATUS "  2. Add user to group: sudo pw groupmod usb -m $USER")
  #   message(STATUS "  3. Re-login for group changes to take effect")
  #   message(STATUS "")
  #   message(STATUS "Or use convenience target:")
  #   message(STATUS "  sudo cmake --build build --target devd-reload")
  #   message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
  # else()
  #   message(STATUS "No devd rules found in rules/devd/ directory")
  # endif()
  
  # For now, just inform the user
  message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
  message(STATUS "FreeBSD Detected")
  message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
  message(STATUS "  ⚠️  devd rules support not yet implemented")
  message(STATUS "")
  message(STATUS "USB device permissions on FreeBSD:")
  message(STATUS "  1. Add yourself to the usb group:")
  message(STATUS "     sudo pw groupmod usb -m $USER")
  message(STATUS "")
  message(STATUS "  2. Or create devd rules in /usr/local/etc/devd/")
  message(STATUS "     See rules/CMakeLists.txt for template")
  message(STATUS "")
  message(STATUS "  3. Restart devd: sudo service devd restart")
  message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")

# ==================== OTHER UNIX SYSTEMS ====================
else()
  message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
  message(STATUS "${CMAKE_SYSTEM_NAME} Detected")
  message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
  message(STATUS "  ℹ️  USB device permission management not configured")
  message(STATUS "")
  message(STATUS "udev rules for Linux are available in:")
  message(STATUS "  ${CMAKE_CURRENT_SOURCE_DIR}")
  message(STATUS "")
  message(STATUS "You may need to configure USB permissions manually")
  message(STATUS "for your operating system.")
  message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
endif()

# ==================== LIST AVAILABLE RULES ====================
# Show what's available regardless of platform
file(GLOB ALL_RULE_FILES "*.rules" "devd/*.conf")

if(ALL_RULE_FILES)
  message(STATUS "")
  message(STATUS "Available device rules in this directory:")
  
  # Count by type
  file(GLOB UDEV_RULE_FILES "*.rules")
  file(GLOB DEVD_RULE_FILES "devd/*.conf")
  
  list(LENGTH UDEV_RULE_FILES _num_udev)
  list(LENGTH DEVD_RULE_FILES _num_devd)
  
  if(_num_udev GREATER 0)
    message(STATUS "  udev rules (.rules): ${_num_udev} files")
    foreach(rule_file IN LISTS UDEV_RULE_FILES)
      get_filename_component(rule_name ${rule_file} NAME)
      # Skip non-rule files
      if(NOT rule_name MATCHES "^(Makefile|README|\\..*)$")
        message(STATUS "    • ${rule_name}")
      endif()
    endforeach()
  endif()
  
  if(_num_devd GREATER 0)
    message(STATUS "  devd rules (.conf): ${_num_devd} files")
    foreach(rule_file IN LISTS DEVD_RULE_FILES)
      get_filename_component(rule_name ${rule_file} NAME)
      message(STATUS "    • ${rule_name}")
    endforeach()
  endif()
  
  message(STATUS "")
endif()

# ==================== SUPPORTED SDR DEVICES ====================
# Document which SDR devices are covered by these rules
message(STATUS "SDR devices supported by these rules:")
message(STATUS "  • Airspy R2/Mini (52-airspy.rules)")
message(STATUS "  • Airspy HF+ (52-airspyhf.rules)")
message(STATUS "  • RTL-SDR dongles (20-rtlsdr.rules)")
message(STATUS "  • HackRF One (66-hackrf.rules)")
message(STATUS "  • Funcube Dongle (68-funcube-dongle.rules)")
message(STATUS "  • Funcube Dongle Pro+ (68-funcube-dongle-proplus.rules)")
message(STATUS "  • Fobos SDR (73-fobos-sdr.rules)")
message(STATUS "  • RX888 (99-rx888.rules)")
message(STATUS "")

# ==================== IMPLEMENTATION NOTES ====================
#
# UDEV RULES (Linux)
# ------------------
# ✅ IMPLEMENTED
# Location: /etc/udev/rules.d/*.rules (or /usr/lib/udev/rules.d/*.rules)
# Purpose: Set device permissions, create symlinks, set group ownership
# Format: ATTR{idVendor}=="...", ATTR{idProduct}=="...", MODE="660", GROUP="plugdev"
# Reference: https://www.reactivated.net/writing_udev_rules.html
#
# DEVD RULES (FreeBSD)
# --------------------
# ⚠️  TODO - Template provided above
# Location: /usr/local/etc/devd/*.conf
# Purpose: Set device permissions, ownership on device attach/detach
# Format: attach/detach blocks with match conditions and actions
# Reference: https://www.freebsd.org/cgi/man.cgi?query=devd.conf
#
# MACOS
# -----
# ℹ️  NOT APPLICABLE
# macOS doesn't use udev or devd. USB devices are typically accessible
# to all users by default. No special configuration needed.
#
# EXAMPLE DIRECTORY STRUCTURE:
# ----------------------------
# rules/
# ├── CMakeLists.txt          # This file
# ├── README                  # Documentation
# ├── *.rules                 # udev rules (Linux)
# └── devd/                   # devd rules (FreeBSD) - TODO
#     ├── airspy.conf
#     ├── hackrf.conf
#     └── ...
#
