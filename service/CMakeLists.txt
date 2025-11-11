# service/CMakeLists.txt
# Installation of service/daemon management files for ka9q-radio
#
# Supports:
#   - Linux: systemd .service files → /etc/systemd/system
#   - macOS: launchd .plist files → /Library/LaunchDaemons (TODO)
#   - FreeBSD: rc.d scripts → /usr/local/etc/rc.d (TODO)

# ==================== LINUX: SYSTEMD SERVICES ====================
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # Get list of all .service files in this directory
  file(GLOB SERVICE_FILES "*.service")
  
  # Check if systemd is available
  find_program(SYSTEMCTL_EXECUTABLE systemctl)
  
  if(SYSTEMCTL_EXECUTABLE)
    # Install service files to systemd directory
    install(FILES ${SERVICE_FILES}
            DESTINATION /etc/systemd/system
            PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
    
    # Create a custom target for reloading systemd
    # Note: This won't run automatically during install (requires sudo)
    # Users must run: sudo cmake --build build --target systemd-reload
    add_custom_target(systemd-reload
      COMMAND ${SYSTEMCTL_EXECUTABLE} daemon-reload
      COMMENT "Reloading systemd daemon"
      VERBATIM
    )
    
    message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
    message(STATUS "Systemd Service Files")
    message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
    message(STATUS "  Install location: /etc/systemd/system")
    message(STATUS "  Service count: ${CMAKE_MATCH_COUNT}")
    message(STATUS "")
    message(STATUS "After installation:")
    message(STATUS "  1. Reload systemd: sudo systemctl daemon-reload")
    message(STATUS "  2. Start service:  sudo systemctl start <service-name>")
    message(STATUS "  3. Enable at boot: sudo systemctl enable <service-name>")
    message(STATUS "")
    message(STATUS "Or use convenience target:")
    message(STATUS "  sudo cmake --build build --target systemd-reload")
    message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
  else()
    message(WARNING "systemctl not found - skipping systemd service installation")
    message(STATUS "Install systemd if you want service file support")
  endif()

# ==================== MACOS: LAUNCHD PLISTS ====================
elseif(APPLE)
  # ┌─────────────────────────────────────────────────────────────┐
  # │ TODO: Add macOS launchd support                            │
  # │                                                             │
  # │ To implement:                                               │
  # │ 1. Create .plist files in this directory for each daemon   │
  # │    Example: org.ka9q.radio.radiod.plist                    │
  # │                                                             │
  # │ 2. Uncomment and configure the code below                  │
  # │                                                             │
  # │ 3. Test with: sudo cmake --install build                   │
  # │    Then: sudo launchctl load <plist-file>                  │
  # └─────────────────────────────────────────────────────────────┘
  
  # Uncomment when .plist files are available:
  # file(GLOB PLIST_FILES "*.plist")
  # 
  # if(PLIST_FILES)
  #   install(FILES ${PLIST_FILES}
  #           DESTINATION /Library/LaunchDaemons
  #           PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
  #   
  #   message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
  #   message(STATUS "macOS Launchd Property Lists")
  #   message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
  #   message(STATUS "  Install location: /Library/LaunchDaemons")
  #   message(STATUS "")
  #   message(STATUS "After installation:")
  #   message(STATUS "  1. Load daemon:   sudo launchctl load /Library/LaunchDaemons/<plist>")
  #   message(STATUS "  2. Start daemon:  sudo launchctl start <label>")
  #   message(STATUS "  3. Stop daemon:   sudo launchctl stop <label>")
  #   message(STATUS "  4. Unload daemon: sudo launchctl unload /Library/LaunchDaemons/<plist>")
  #   message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
  # else()
  #   message(STATUS "No launchd .plist files found in service/ directory")
  # endif()
  
  # For now, just inform the user
  message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
  message(STATUS "macOS Detected")
  message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
  message(STATUS "  ⚠️  Launchd .plist support not yet implemented")
  message(STATUS "")
  message(STATUS "To add macOS daemon support:")
  message(STATUS "  1. Create .plist files in service/ directory")
  message(STATUS "  2. Uncomment launchd section in service/CMakeLists.txt")
  message(STATUS "  3. See service/CMakeLists.txt for implementation template")
  message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")

# ==================== FREEBSD: RC.D SCRIPTS ====================
elseif(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
  # ┌─────────────────────────────────────────────────────────────┐
  # │ TODO: Add FreeBSD rc.d support                             │
  # │                                                             │
  # │ To implement:                                               │
  # │ 1. Create rc.d scripts in this directory for each daemon   │
  # │    Example: radiod (executable shell script)               │
  # │                                                             │
  # │ 2. Uncomment and configure the code below                  │
  # │                                                             │
  # │ 3. Test with: sudo cmake --install build                   │
  # │    Then: sudo service radiod start                         │
  # └─────────────────────────────────────────────────────────────┘
  
  # Uncomment when rc.d scripts are available:
  # file(GLOB RCD_SCRIPTS "rc.d/*")
  # 
  # if(RCD_SCRIPTS)
  #   install(PROGRAMS ${RCD_SCRIPTS}
  #           DESTINATION /usr/local/etc/rc.d
  #           PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
  #                      GROUP_READ GROUP_EXECUTE
  #                      WORLD_READ WORLD_EXECUTE)
  #   
  #   message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
  #   message(STATUS "FreeBSD rc.d Scripts")
  #   message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
  #   message(STATUS "  Install location: /usr/local/etc/rc.d")
  #   message(STATUS "")
  #   message(STATUS "After installation:")
  #   message(STATUS "  1. Enable service: echo 'radiod_enable=\"YES\"' >> /etc/rc.conf")
  #   message(STATUS "  2. Start service:  sudo service radiod start")
  #   message(STATUS "  3. Stop service:   sudo service radiod stop")
  #   message(STATUS "  4. Check status:   sudo service radiod status")
  #   message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
  # else()
  #   message(STATUS "No rc.d scripts found in service/rc.d/ directory")
  # endif()
  
  # For now, just inform the user
  message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
  message(STATUS "FreeBSD Detected")
  message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
  message(STATUS "  ⚠️  rc.d script support not yet implemented")
  message(STATUS "")
  message(STATUS "To add FreeBSD daemon support:")
  message(STATUS "  1. Create rc.d scripts in service/rc.d/ directory")
  message(STATUS "  2. Uncomment rc.d section in service/CMakeLists.txt")
  message(STATUS "  3. See service/CMakeLists.txt for implementation template")
  message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")

# ==================== OTHER UNIX SYSTEMS ====================
else()
  message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
  message(STATUS "${CMAKE_SYSTEM_NAME} Detected")
  message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
  message(STATUS "  ℹ️  Daemon/service management not configured for this OS")
  message(STATUS "")
  message(STATUS "Service files for Linux systemd are available in:")
  message(STATUS "  ${CMAKE_CURRENT_SOURCE_DIR}")
  message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
endif()

# ==================== LIST AVAILABLE SERVICE DEFINITIONS ====================
# Show what's available regardless of platform
file(GLOB ALL_SERVICE_DEFS "*.service" "*.plist" "rc.d/*")

if(ALL_SERVICE_DEFS)
  message(STATUS "")
  message(STATUS "Available service definitions in this directory:")
  
  # Count by type
  file(GLOB SYSTEMD_SERVICES "*.service")
  file(GLOB LAUNCHD_PLISTS "*.plist")
  file(GLOB RCD_SCRIPTS "rc.d/*")
  
  list(LENGTH SYSTEMD_SERVICES _num_systemd)
  list(LENGTH LAUNCHD_PLISTS _num_launchd)
  list(LENGTH RCD_SCRIPTS _num_rcd)
  
  if(_num_systemd GREATER 0)
    message(STATUS "  systemd (.service): ${_num_systemd} files")
    foreach(service_file IN LISTS SYSTEMD_SERVICES)
      get_filename_component(service_name ${service_file} NAME)
      message(STATUS "    • ${service_name}")
    endforeach()
  endif()
  
  if(_num_launchd GREATER 0)
    message(STATUS "  launchd (.plist): ${_num_launchd} files")
    foreach(plist_file IN LISTS LAUNCHD_PLISTS)
      get_filename_component(plist_name ${plist_file} NAME)
      message(STATUS "    • ${plist_name}")
    endforeach()
  endif()
  
  if(_num_rcd GREATER 0)
    message(STATUS "  rc.d scripts: ${_num_rcd} files")
    foreach(rcd_file IN LISTS RCD_SCRIPTS)
      get_filename_component(rcd_name ${rcd_file} NAME)
      message(STATUS "    • ${rcd_name}")
    endforeach()
  endif()
  
  message(STATUS "")
endif()

# ==================== IMPLEMENTATION NOTES ====================
#
# SYSTEMD (.service files for Linux)
# ----------------------------------
# ✅ IMPLEMENTED
# Location: /etc/systemd/system/*.service
# Management: systemctl start/stop/enable/disable <service>
#
# LAUNCHD (.plist files for macOS)
# ---------------------------------
# ⚠️  TODO - Template provided above
# Location: /Library/LaunchDaemons/*.plist
# Management: launchctl load/unload/start/stop <plist>
# Format: XML property list with Label, ProgramArguments, RunAtLoad, etc.
# Reference: https://www.launchd.info/
#
# RC.D (shell scripts for FreeBSD)
# ---------------------------------
# ⚠️  TODO - Template provided above
# Location: /usr/local/etc/rc.d/<script>
# Management: service <script> start/stop/status
# Format: Shell script with rc.subr framework
# Reference: https://docs.freebsd.org/en/articles/rc-scripting/
#
# EXAMPLE DIRECTORY STRUCTURE:
# ----------------------------
# service/
# ├── CMakeLists.txt          # This file
# ├── *.service               # systemd services (Linux)
# ├── *.plist                 # launchd plists (macOS) - TODO
# └── rc.d/                   # rc.d scripts (FreeBSD) - TODO
#     ├── radiod
#     ├── packetd
#     └── ...
#
