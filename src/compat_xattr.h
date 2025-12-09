#pragma once
#include <sys/types.h>

/* ========= Linux: native xattr API ========= */
#if defined(__linux__)
  #include <sys/xattr.h>

/* ========= macOS: xattr exists but f* variants have extra args ========= */
#elif defined(__APPLE__)
  #include <sys/xattr.h>
  /* Map Linux-like 4-arg calls to Darwin's 6-arg prototypes (position=0, options=0) */
  static inline ssize_t kr_fgetxattr(int fd, const char *name, void *value, size_t size) {
    return fgetxattr(fd, name, value, size, 0, 0);
  }
  static inline int kr_fsetxattr(int fd, const char *name, const void *value, size_t size, int flags) {
    return fsetxattr(fd, name, value, size, 0, flags);
  }
  static inline int kr_fremovexattr(int fd, const char *name) {
    return fremovexattr(fd, name, 0);
  }
  /* Keep call sites simple: alias to our wrappers */
  #define fgetxattr   kr_fgetxattr
  #define fsetxattr   kr_fsetxattr
  #define fremovexattr kr_fremovexattr

/* ========= FreeBSD: extattr(2) instead of xattr ========= */
#elif defined(__FreeBSD__)
  #include <sys/extattr.h>
  static inline ssize_t fgetxattr(int fd, const char *name, void *value, size_t size) {
    return extattr_get_fd(fd, EXTATTR_NAMESPACE_USER, name, value, size);
  }
  static inline int fsetxattr(int fd, const char *name, const void *value, size_t size, int flags) {
    (void)flags; /* extattr doesn't take flags; ignore for compat */
    return extattr_set_fd(fd, EXTATTR_NAMESPACE_USER, name, value, size);
  }
  static inline int fremovexattr(int fd, const char *name) {
    return extattr_delete_fd(fd, EXTATTR_NAMESPACE_USER, name);
  }

/* ========= Fallback: try to use sys/xattr.h if present ========= */
#else
  #ifdef __has_include
    #if __has_include(<sys/xattr.h>)
      #include <sys/xattr.h>
    #else
      #error "No extended attributes API found on this platform"
    #endif
  #else
    #include <sys/xattr.h>
  #endif
#endif

