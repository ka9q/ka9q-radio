include_guard(GLOBAL)                       # prevent double inclusion
include(CheckIncludeFiles)

check_include_files("sys/socket.h;netinet/in.h" HAVE_SOCKADDR_IN)
check_include_files("arpa/inet.h" HAVE_ARPA_INET_H)

configure_file(
  "${CMAKE_SOURCE_DIR}/cmake/ka9q_config.h.in"
  "${CMAKE_BINARY_DIR}/generated/ka9q_config.h"
  @ONLY
)

# Prefer target-scoped includes if possible; global is acceptable early on:
include_directories("${CMAKE_BINARY_DIR}/generated")

