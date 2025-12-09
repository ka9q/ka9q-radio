# docs/CMakeLists.txt — placeholder/no-op

# Toggle for future doc generation (currently unused)
option(BUILD_DOCS "Build project documentation" OFF)

# Always provide a 'docs' target so `make docs` / `ninja docs` works.
add_custom_target(docs
  COMMAND ${CMAKE_COMMAND} -E echo "No documentation to build in this tree."
  COMMENT "docs: placeholder target (nothing to do)"
  VERBATIM
)

# If you later add real doc rules, hook them behind BUILD_DOCS
# and make 'docs' depend on them:
if(BUILD_DOCS)
  # add_custom_target(docs-build ...)  # your real rules go here
  # add_dependencies(docs docs-build)
endif()

# Optional: install prebuilt static docs if a folder exists (safe no-op otherwise).
# Requires top-level to include(GNUInstallDirs) so CMAKE_INSTALL_DOCDIR is defined.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/_site")
  install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/_site/"
          DESTINATION "${CMAKE_INSTALL_DOCDIR}"
          OPTIONAL)
endif()

