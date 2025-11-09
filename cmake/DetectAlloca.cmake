# cmake/DetectAlloca.cmake
# Detect whether <alloca.h> exists (Linux/glibc). On BSD/macOS alloca()
# is declared in <stdlib.h> and <alloca.h> is typically absent.

include_guard(GLOBAL)
include(CheckIncludeFiles)

check_include_files("alloca.h" HAVE_ALLOCA_H)

# Refresh the generated feature header (merged with other detectors)
configure_file(
  "${CMAKE_SOURCE_DIR}/cmake/ka9q_config.h.in"
  "${CMAKE_BINARY_DIR}/generated/ka9q_config.h"
  @ONLY
)

