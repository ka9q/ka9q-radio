# share/CMakeLists.txt — install runtime data files

# Requires GNUInstallDirs from the top-level:
#   include(GNUInstallDirs)
# so ${CMAKE_INSTALL_DATADIR} is available.

# Optionally let callers choose to skip overwriting existing files at install time
option(KA9Q_SHARE_SKIP_EXISTING "Do not overwrite files that already exist at the install destination" OFF)

# Where to install these files (e.g., /usr/local/share/ka9q-radio)
set(KA9Q_SHARE_INSTALL_DIR "${CMAKE_INSTALL_DATADIR}/ka9q-radio" CACHE PATH
    "Destination for ka9q-radio share files")

# -------------------------------
# Variant A: Standard CMake install (overwrites existing files)
# -------------------------------
if(NOT KA9Q_SHARE_SKIP_EXISTING)
  install(
    DIRECTORY
      ${CMAKE_CURRENT_SOURCE_DIR}/
    DESTINATION
      ${KA9Q_SHARE_INSTALL_DIR}
    FILES_MATCHING
      PATTERN "*"
      # Exclusions to avoid copying build system files or VCS metadata
      PATTERN "CMakeLists.txt" EXCLUDE
      PATTERN ".git*"          EXCLUDE
      PATTERN ".DS_Store"      EXCLUDE
  )

else()
  # -------------------------------
  # Variant B: Emulate `rsync --ignore-existing`
  # Copies only files that DO NOT already exist at the destination.
  # -------------------------------
  # Build a list of all source files to consider
  file(GLOB_RECURSE KA9Q_SHARE_SRC_FILES
    LIST_DIRECTORIES FALSE
    "${CMAKE_CURRENT_SOURCE_DIR}/*"
  )

  # Filter out things we don't want to install
  set(_filtered)
  foreach(f IN LISTS KA9Q_SHARE_SRC_FILES)
    get_filename_component(name "${f}" NAME)
    if(name STREQUAL "CMakeLists.txt")          # exclude
      continue()
    elseif(name MATCHES "^\\.git")               # exclude .git, .gitignore, etc
      continue()
    elseif(name STREQUAL ".DS_Store")            # exclude macOS junk
      continue()
    endif()
    list(APPEND _filtered "${f}")
  endforeach()

  # At install time, copy only files that don't already exist
  # (This runs during "cmake --install" or packaging)
  install(CODE
    "
    file(MAKE_DIRECTORY \"\$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/${KA9Q_SHARE_INSTALL_DIR}\")
    foreach(src \"${_filtered}\")
      # Compute relative path under share/
      file(RELATIVE_PATH rel \"${CMAKE_CURRENT_SOURCE_DIR}\" \"\${src}\")
      set(dst \"\$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/${KA9Q_SHARE_INSTALL_DIR}/\${rel}\")
      if(NOT EXISTS \"\${dst}\")
        # Ensure parent directory exists
        get_filename_component(dst_dir \"\${dst}\" DIRECTORY)
        file(MAKE_DIRECTORY \"\${dst_dir}\")
        file(INSTALL
          DESTINATION \"\${dst_dir}\"
          TYPE FILE
          FILES \"\${src}\")
      endif()
    endforeach()
    "
  )
endif()

# -------------------------------
# Ownership/group note
# -------------------------------
# If you *must* set ownership like the original Makefile:
#  - Prefer to do this in your system packaging (deb/rpm) spec.
#  - Or add a guarded post-install step (requires privileges), e.g.:
#
# install(CODE "
#   if(UNIX AND NOT APPLE)
#     # chown -R radio:radio <dir>  (Linux)
#     execute_process(COMMAND chown -R radio:radio
#       \"$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/${KA9Q_SHARE_INSTALL_DIR}\"
#       RESULT_VARIABLE rv)
#   elseif(APPLE)
#     # chgrp -R radio <dir>  (macOS)
#     execute_process(COMMAND chgrp -R radio
#       \"$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/${KA9Q_SHARE_INSTALL_DIR}\"
#       RESULT_VARIABLE rv)
#   endif()
# ")
#
# Use with care—packagers generally frown on installers changing ownership.

