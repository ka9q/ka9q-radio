// Portable version of fallocate to run on both Linux and macos
// I think I wrote this...
// Copyright 2026 Phil Karn, KA9Q

#define _GNU_SOURCE 1
int portable_fallocate(int fd, off_t size) {
#ifdef __APPLE__
    fstore_t store = {
        .fst_flags = F_ALLOCATECONTIG,
        .fst_posmode = F_PEOFPOSMODE,
        .fst_offset = 0,
        .fst_length = size,
        .fst_bytesalloc = 0
    };
    if (fcntl(fd, F_PREALLOCATE, &store) == -1) {
        store.fst_flags = F_ALLOCATEALL;
        if (fcntl(fd, F_PREALLOCATE, &store) == -1)
            return -1;
    }
    return ftruncate(fd, size);
#else
    return posix_fallocate(fd, 0, size);
#endif
}
