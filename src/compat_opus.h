#pragma once

#if __has_include(<opus/opus.h>)
  #include <opus/opus.h>   // Homebrew, most Linux distros, FreeBSD ports
#elif __has_include(<opus.h>)
  #include <opus.h>        // Rare flat installs
#else
  #error "Opus headers not found. Install opus (e.g. brew install opus)."
#endif

