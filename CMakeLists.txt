cmake_minimum_required(VERSION 3.16)
project(ka9q-radio VERSION 0.1 LANGUAGES C)

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
