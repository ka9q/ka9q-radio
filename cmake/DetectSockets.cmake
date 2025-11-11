# cmake/DetectSockets.cmake
# Detect socket-related headers and generate config header
include_guard(GLOBAL)

include(CheckIncludeFiles)

# Check for socket headers (should be available on all POSIX systems)
check_include_files("sys/socket.h;netinet/in.h" HAVE_SOCKADDR_IN)
check_include_files("arpa/inet.h" HAVE_ARPA_INET_H)

# Generate the configuration header
configure_file(
  "${CMAKE_SOURCE_DIR}/cmake/ka9q_config.h.in"
  "${CMAKE_BINARY_DIR}/generated/ka9q_config.h"
  @ONLY
)

message(STATUS "Generated config header: ${CMAKE_BINARY_DIR}/generated/ka9q_config.h")
