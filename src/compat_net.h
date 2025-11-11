#pragma once
#include "ka9q_config.h"

#include <sys/types.h>
#include <sys/socket.h>
#if HAVE_SOCKADDR_IN
  #include <netinet/in.h>
#endif
#if HAVE_ARPA_INET_H
  #include <arpa/inet.h>
#endif
