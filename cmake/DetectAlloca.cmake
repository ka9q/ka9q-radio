# cmake/DetectAlloca.cmake
# Detect whether <alloca.h> exists
# On Linux/glibc, alloca() is declared in <alloca.h>
# On BSD/macOS, alloca() is in <stdlib.h> and <alloca.h> typically doesn't exist

include_guard(GLOBAL)
include(CheckIncludeFiles)

check_include_files("alloca.h" HAVE_ALLOCA_H)

if(HAVE_ALLOCA_H)
  message(STATUS "Found alloca.h")
else()
  message(STATUS "alloca.h not found (will use stdlib.h on BSD/macOS)")
endif()

# Regenerate the configuration header with alloca detection
configure_file(
  "${CMAKE_SOURCE_DIR}/cmake/ka9q_config.h.in"
  "${CMAKE_BINARY_DIR}/generated/ka9q_config.h"
  @ONLY
)
