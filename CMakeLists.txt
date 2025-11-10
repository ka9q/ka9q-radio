cmake_minimum_required(VERSION 3.16)
project(ka9q-radio VERSION 0.1 LANGUAGES C)

# Global universally portable compile flags
add_compile_options(
  -O3
  -march=native
  -std=gnu11
  -Wall
  -Wextra
  -Wno-gnu-folding-constant
  -pthread
  -D_GNU_SOURCE=1
)

# Detect compiler and set math-related flags
if (CMAKE_C_COMPILER_ID STREQUAL "GNU")
    set(MATH_FLAGS
        -funsafe-math-optimizations
        -fno-math-errno
        -fcx-limited-range
        -freciprocal-math
        -fno-trapping-math
    )
elseif (CMAKE_C_COMPILER_ID MATCHES "Clang")
    set(MATH_FLAGS
        -fno-math-errno
        -freciprocal-math
        -fno-trapping-math
    )
else()
    set(MATH_FLAGS "")
endif()

# Pass MATH_FLAGS to subdirectories
set(MATH_FLAGS ${MATH_FLAGS})

# Make our project modules discoverable
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

include(Options)           # options, compiler knobs, FreeBSD helpers
include(DetectSockets)     # header checks + generated config header
include(DetectAlloca)      # header checks + generated config header
include(FindDeps)          # pkg-config dependencies (creates imported targets)
include(DetectLibusbStringDescriptor)

add_subdirectory(src)

# make generated header visible to targets in src
target_include_directories(radio PUBLIC "${CMAKE_BINARY_DIR}/generated")
