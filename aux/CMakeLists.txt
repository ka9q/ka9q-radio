# aux/CMakeLists.txt
# System configuration and auxiliary files for ka9q-radio
#
# This directory contains system-level configuration files including:
#   - User/group creation (sysusers.d)
#   - Directory management (tmpfiles.d)
#   - Kernel tuning (sysctl.d)
#   - Log rotation (logrotate.d)
#   - Kernel module blacklists (modprobe.d)
#   - Utility scripts
#   - Cron jobs
#
# WARNING: These files require root privileges and modify system configuration!

# ==================== LINUX: FULL SYSTEM SETUP ====================
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  
  message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
  message(STATUS "System Configuration Files (aux/)")
  message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
  message(STATUS "")
  message(STATUS "⚠️  These files require root privileges and modify system config!")
  message(STATUS "")
  
  # ===== User/Group Management (systemd-sysusers) =====
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/radio.sysusers")
    install(FILES radio.sysusers
            DESTINATION /etc/sysusers.d
            RENAME radio.conf
            PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
    message(STATUS "  • radio.sysusers → /etc/sysusers.d/radio.conf")
    message(STATUS "    Creates radio user/group (UID/GID 55050)")
  endif()
  
  # ===== Directory Management (systemd-tmpfiles) =====
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/ka9q-radio.tmpfiles")
    install(FILES ka9q-radio.tmpfiles
            DESTINATION /etc/tmpfiles.d
            RENAME ka9q-radio.conf
            PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
    message(STATUS "  • ka9q-radio.tmpfiles → /etc/tmpfiles.d/ka9q-radio.conf")
    message(STATUS "    Creates /var/lib/ka9q-radio, /var/log/ka9q-radio, /run/ka9q-radio")
  endif()
  
  # ===== Kernel Tuning (sysctl) =====
  file(GLOB SYSCTL_FILES "*.conf")
  if(SYSCTL_FILES)
    # Filter out tmpfiles/sysusers/other conf files
    set(_sysctl_files "")
    foreach(_file IN LISTS SYSCTL_FILES)
      get_filename_component(_name ${_file} NAME)
      if(_name MATCHES "^[0-9].*\\.conf$")
        list(APPEND _sysctl_files ${_file})
      endif()
    endforeach()
    
    if(_sysctl_files)
      install(FILES ${_sysctl_files}
              DESTINATION /etc/sysctl.d
              PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
      message(STATUS "  • sysctl configs → /etc/sysctl.d/")
      foreach(_file IN LISTS _sysctl_files)
        get_filename_component(_name ${_file} NAME)
        message(STATUS "    - ${_name}")
      endforeach()
    endif()
  endif()
  
  # ===== Kernel Module Blacklist =====
  file(GLOB BLACKLIST_FILES "*-blacklist.conf")
  if(BLACKLIST_FILES)
    install(FILES ${BLACKLIST_FILES}
            DESTINATION /etc/modprobe.d
            PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
    message(STATUS "  • module blacklists → /etc/modprobe.d/")
    foreach(_file IN LISTS BLACKLIST_FILES)
      get_filename_component(_name ${_file} NAME)
      message(STATUS "    - ${_name}")
    endforeach()
  endif()
  
  # ===== Log Rotation =====
  file(GLOB LOGROTATE_FILES "*.rotate")
  if(LOGROTATE_FILES)
    install(FILES ${LOGROTATE_FILES}
            DESTINATION /etc/logrotate.d
            PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
    message(STATUS "  • log rotation configs → /etc/logrotate.d/")
    foreach(_file IN LISTS LOGROTATE_FILES)
      get_filename_component(_name ${_file} NAME_WE)
      message(STATUS "    - ${_name}")
    endforeach()
  endif()
  
  # ===== Cron Jobs =====
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/ka9q-cleanups")
    install(FILES ka9q-cleanups
            DESTINATION /etc/cron.d
            PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
    message(STATUS "  • ka9q-cleanups → /etc/cron.d/")
  endif()
  
  # ===== Utility Scripts =====
  file(GLOB SCRIPTS "*.sh")
  if(SCRIPTS)
    # Separate scripts by destination
    set(_bin_scripts "")
    set(_sbin_scripts "")
    
    foreach(_script IN LISTS SCRIPTS)
      get_filename_component(_name ${_script} NAME)
      # Scripts starting with "start-" go to sbin
      if(_name MATCHES "^start-")
        list(APPEND _sbin_scripts ${_script})
      else()
        list(APPEND _bin_scripts ${_script})
      endif()
    endforeach()
    
    if(_bin_scripts)
      install(PROGRAMS ${_bin_scripts}
              DESTINATION bin)
      message(STATUS "  • utility scripts → bin/")
      foreach(_script IN LISTS _bin_scripts)
        get_filename_component(_name ${_script} NAME)
        message(STATUS "    - ${_name}")
      endforeach()
    endif()
    
    if(_sbin_scripts)
      install(PROGRAMS ${_sbin_scripts}
              DESTINATION sbin)
      message(STATUS "  • daemon scripts → sbin/")
      foreach(_script IN LISTS _sbin_scripts)
        get_filename_component(_name ${_script} NAME)
        message(STATUS "    - ${_name}")
      endforeach()
    endif()
  endif()
  
  # ===== HFDL Configuration =====
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/systable.conf")
    # Install with OPTIONAL flag - only if file doesn't exist
    install(FILES systable.conf
            DESTINATION /var/lib/hfdl
            PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ
            OPTIONAL)
    message(STATUS "  • systable.conf → /var/lib/hfdl/ (if not exists)")
  endif()
  
  # ===== Create Custom Targets for Post-Install Tasks =====
  
  # Reload systemd-sysusers (create users/groups)
  find_program(SYSTEMD_SYSUSERS systemd-sysusers)
  if(SYSTEMD_SYSUSERS)
    add_custom_target(sysusers-reload
      COMMAND ${SYSTEMD_SYSUSERS}
      COMMENT "Creating radio user and group"
      VERBATIM
    )
  endif()
  
  # Reload systemd-tmpfiles (create directories)
  find_program(SYSTEMD_TMPFILES systemd-tmpfiles)
  if(SYSTEMD_TMPFILES)
    add_custom_target(tmpfiles-reload
      COMMAND ${SYSTEMD_TMPFILES} --create
      COMMENT "Creating ka9q-radio directories"
      VERBATIM
    )
  endif()
  
  # Reload sysctl settings
  find_program(SYSCTL_EXECUTABLE sysctl)
  if(SYSCTL_EXECUTABLE)
    add_custom_target(sysctl-reload
      COMMAND ${SYSCTL_EXECUTABLE} --system
      COMMENT "Reloading kernel parameters"
      VERBATIM
    )
  endif()
  
  # Combined setup target
  add_custom_target(system-setup
    COMMENT "Running all post-install setup tasks"
    VERBATIM
  )
  if(SYSTEMD_SYSUSERS)
    add_dependencies(system-setup sysusers-reload)
  endif()
  if(SYSTEMD_TMPFILES)
    add_dependencies(system-setup tmpfiles-reload)
  endif()
  if(SYSCTL_EXECUTABLE)
    add_dependencies(system-setup sysctl-reload)
  endif()
  
  message(STATUS "")
  message(STATUS "After installation, run these commands:")
  message(STATUS "  1. Create user/group:  sudo systemd-sysusers")
  message(STATUS "  2. Create directories: sudo systemd-tmpfiles --create")
  message(STATUS "  3. Apply sysctl:       sudo sysctl --system")
  message(STATUS "  4. Reload modules:     sudo modprobe -r airspy (if needed)")
  message(STATUS "")
  message(STATUS "Or use convenience target:")
  message(STATUS "  sudo cmake --build build --target system-setup")
  message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")

# ==================== MACOS: PARTIAL SETUP ====================
elseif(APPLE)
  message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
  message(STATUS "macOS System Configuration (aux/)")
  message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
  message(STATUS "")
  
  # ===== Utility Scripts =====
  file(GLOB SCRIPTS "*.sh")
  if(SCRIPTS)
    set(_bin_scripts "")
    set(_sbin_scripts "")
    
    foreach(_script IN LISTS SCRIPTS)
      get_filename_component(_name ${_script} NAME)
      if(_name MATCHES "^start-")
        list(APPEND _sbin_scripts ${_script})
      else()
        list(APPEND _bin_scripts ${_script})
      endif()
    endforeach()
    
    if(_bin_scripts)
      install(PROGRAMS ${_bin_scripts} DESTINATION bin)
      message(STATUS "  • utility scripts → bin/")
    endif()
    
    if(_sbin_scripts)
      install(PROGRAMS ${_sbin_scripts} DESTINATION sbin)
      message(STATUS "  • daemon scripts → sbin/")
    endif()
  endif()
  
  # ===== HFDL Configuration =====
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/systable.conf")
    install(FILES systable.conf
            DESTINATION /var/lib/hfdl
            PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ
            OPTIONAL)
    message(STATUS "  • systable.conf → /var/lib/hfdl/")
  endif()
  
  message(STATUS "")
  message(STATUS "⚠️  Manual setup required on macOS:")
  message(STATUS "  1. Create radio group:")
  message(STATUS "     sudo dscl . -create /Groups/radio PrimaryGroupID 492")
  message(STATUS "")
  message(STATUS "  2. Create directories:")
  message(STATUS "     sudo mkdir -p /var/lib/ka9q-radio /var/log/ka9q-radio")
  message(STATUS "     sudo chgrp -R radio /var/lib/ka9q-radio /var/log/ka9q-radio")
  message(STATUS "     sudo chmod -R g+ws /var/lib/ka9q-radio /var/log/ka9q-radio")
  message(STATUS "")
  message(STATUS "  3. Add yourself to radio group:")
  message(STATUS "     sudo dscl . -append /Groups/radio GroupMembership $USER")
  message(STATUS "")
  message(STATUS "  4. Log out and back in for group changes to take effect")
  message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")

# ==================== FREEBSD: PARTIAL SETUP ====================
elseif(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
  message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
  message(STATUS "FreeBSD System Configuration (aux/)")
  message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
  message(STATUS "")
  
  # ===== Utility Scripts =====
  file(GLOB SCRIPTS "*.sh")
  if(SCRIPTS)
    set(_bin_scripts "")
    set(_sbin_scripts "")
    
    foreach(_script IN LISTS SCRIPTS)
      get_filename_component(_name ${_script} NAME)
      if(_name MATCHES "^start-")
        list(APPEND _sbin_scripts ${_script})
      else()
        list(APPEND _bin_scripts ${_script})
      endif()
    endforeach()
    
    if(_bin_scripts)
      install(PROGRAMS ${_bin_scripts} DESTINATION bin)
      message(STATUS "  • utility scripts → bin/")
    endif()
    
    if(_sbin_scripts)
      install(PROGRAMS ${_sbin_scripts} DESTINATION sbin)
      message(STATUS "  • daemon scripts → sbin/")
    endif()
  endif()
  
  # ===== HFDL Configuration =====
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/systable.conf")
    install(FILES systable.conf
            DESTINATION /var/lib/hfdl
            PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ
            OPTIONAL)
    message(STATUS "  • systable.conf → /var/lib/hfdl/")
  endif()
  
  # ===== Kernel Tuning (sysctl.conf) =====
  # Note: FreeBSD sysctl format is different from Linux
  message(STATUS "")
  message(STATUS "⚠️  sysctl tuning available but not auto-installed")
  message(STATUS "    (Linux sysctl.d format differs from FreeBSD)")
  
  message(STATUS "")
  message(STATUS "Manual setup required on FreeBSD:")
  message(STATUS "  1. Create radio group:")
  message(STATUS "     sudo pw groupadd radio -g 55050")
  message(STATUS "")
  message(STATUS "  2. Create radio user:")
  message(STATUS "     sudo pw useradd radio -u 55050 -g radio -d /var/lib/ka9q-radio -s /usr/sbin/nologin")
  message(STATUS "")
  message(STATUS "  3. Create directories:")
  message(STATUS "     sudo mkdir -p /var/lib/ka9q-radio /var/log/ka9q-radio")
  message(STATUS "     sudo chown -R radio:radio /var/lib/ka9q-radio /var/log/ka9q-radio")
  message(STATUS "     sudo chmod -R 750 /var/lib/ka9q-radio /var/log/ka9q-radio")
  message(STATUS "")
  message(STATUS "  4. Apply kernel tuning (add to /etc/sysctl.conf):")
  message(STATUS "     kern.ipc.maxsockbuf=5242880")
  message(STATUS "     net.inet.udp.recvspace=5242880")
  message(STATUS "")
  message(STATUS "  5. Add yourself to radio group:")
  message(STATUS "     sudo pw groupmod radio -m $USER")
  message(STATUS "")
  message(STATUS "  6. Log out and back in for group changes to take effect")
  message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")

# ==================== OTHER SYSTEMS ====================
else()
  message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
  message(STATUS "${CMAKE_SYSTEM_NAME} System Configuration")
  message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
  message(STATUS "  ℹ️  System configuration not automated for this OS")
  message(STATUS "")
  message(STATUS "Configuration files are available in:")
  message(STATUS "  ${CMAKE_CURRENT_SOURCE_DIR}")
  message(STATUS "")
  message(STATUS "You may need to manually:")
  message(STATUS "  1. Create radio user/group")
  message(STATUS "  2. Create /var/lib/ka9q-radio directory")
  message(STATUS "  3. Tune kernel parameters for SDR performance")
  message(STATUS "  4. Set up log rotation")
  message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
endif()

# ==================== DOCUMENTATION ====================
message(STATUS "")
message(STATUS "Auxiliary files include:")
message(STATUS "  • User/group management (sysusers.d)")
message(STATUS "  • Directory creation (tmpfiles.d)")
message(STATUS "  • Kernel parameter tuning (sysctl.d)")
message(STATUS "  • Kernel module blacklists (modprobe.d)")
message(STATUS "  • Log rotation configuration")
message(STATUS "  • Utility scripts")
message(STATUS "  • Cron jobs for maintenance")
message(STATUS "  • HFDL configuration")
message(STATUS "")

# ==================== IMPLEMENTATION NOTES ====================
#
# SYSTEM CONFIGURATION FILES
# --------------------------
# This directory contains files that integrate ka9q-radio with the system:
#
# 1. SYSUSERS.D (Linux)
#    - Creates radio user (UID 55050) and group (GID 55050)
#    - Managed by systemd-sysusers
#    - Home: /var/lib/ka9q-radio
#    - Shell: /usr/sbin/nologin (system user)
#
# 2. TMPFILES.D (Linux)
#    - Creates required directories with proper ownership
#    - Managed by systemd-tmpfiles
#    - Directories: /var/lib/ka9q-radio, /var/log/ka9q-radio, /run/ka9q-radio
#
# 3. SYSCTL.D (Linux)
#    - Tunes kernel parameters for SDR performance
#    - 98-sockbuf.conf: Increases socket buffer sizes
#    - 50-multicast.conf: Enables multicast routing
#
# 4. MODPROBE.D (Linux)
#    - Blacklists conflicting kernel modules
#    - airspy-blacklist.conf: Prevents kernel DVB drivers from claiming Airspy
#
# 5. LOGROTATE.D (Linux)
#    - Manages log file rotation for various daemons
#    - Prevents log files from consuming excessive disk space
#
# 6. CRON.D (Linux)
#    - Scheduled maintenance tasks
#    - ka9q-cleanups: Periodic cleanup of temporary files
#
# 7. SCRIPTS
#    - set_lo_multicast.sh: Sets up local multicast routing
#    - start-hfdl.sh: Starts HFDL decoder daemon
#    - start-ka9q-horus.sh: Starts Horus balloon telemetry decoder
#
# 8. HFDL CONFIGURATION
#    - systable.conf: HFDL ground station table
#    - Only installed if file doesn't exist (preserves user changes)
#
# SECURITY CONSIDERATIONS
# -----------------------
# - radio user runs with minimal privileges (no login shell)
# - Directories have 0750 permissions (owner + group only)
# - Configuration files owned by root
# - Scripts installed with appropriate execute permissions
#
# PORTABILITY
# -----------
# - Linux: Full automation via systemd
# - macOS: Manual setup required (different user/group system)
# - FreeBSD: Manual setup required (different init system)
# - Other: Manual setup required
#
