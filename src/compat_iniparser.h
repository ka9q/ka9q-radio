#pragma once

/* Prefer the subdir form used by Homebrew and many distros */
#if __has_include(<iniparser/iniparser.h>)
  #include <iniparser/iniparser.h>
#elif __has_include(<iniparser.h>)
  /* Some systems install it flat */
  #include <iniparser.h>
#else
  #error "iniparser headers not found. Install iniparser (e.g. brew install iniparser)."
#endif

