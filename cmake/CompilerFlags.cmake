# cmake/CompilerFlags.cmake
# Centralized compiler flag management using interface library
include_guard(GLOBAL)

# Create interface library for compile options
add_library(ka9q_compile_flags INTERFACE)

# ==================== WARNING FLAGS ====================
target_compile_options(ka9q_compile_flags INTERFACE
  $<$<OR:$<C_COMPILER_ID:GNU>,$<C_COMPILER_ID:Clang>,$<C_COMPILER_ID:AppleClang>>:-Wall -Wextra>
  $<$<C_COMPILER_ID:MSVC>:/W4>
)

# Clang-specific warnings
if(CMAKE_C_COMPILER_ID MATCHES "Clang")
  target_compile_options(ka9q_compile_flags INTERFACE
    -Wno-gnu-folding-constant
  )
endif()

# ==================== OPTIMIZATION FLAGS ====================
# These respect CMAKE_BUILD_TYPE instead of hardcoding -O3
target_compile_options(ka9q_compile_flags INTERFACE
  $<$<AND:$<CONFIG:Release>,$<C_COMPILER_ID:GNU,Clang,AppleClang>>:-O3>
  $<$<AND:$<CONFIG:Debug>,$<C_COMPILER_ID:GNU,Clang,AppleClang>>:-O0 -g>
  $<$<AND:$<CONFIG:RelWithDebInfo>,$<C_COMPILER_ID:GNU,Clang,AppleClang>>:-O2 -g>
  $<$<AND:$<CONFIG:MinSizeRel>,$<C_COMPILER_ID:GNU,Clang,AppleClang>>:-Os>
  $<$<AND:$<CONFIG:Release>,$<C_COMPILER_ID:MSVC>>:/O2>
  $<$<AND:$<CONFIG:Debug>,$<C_COMPILER_ID:MSVC>>:/Od /Zi>
)

# ==================== NATIVE ARCHITECTURE ====================
# Make -march=native optional (disabled by default for portability)
option(ENABLE_NATIVE_ARCH "Optimize for native CPU architecture (breaks portability, may fail cross-compilation)" OFF)

if(ENABLE_NATIVE_ARCH)
  message(STATUS "Enabling -march=native (build will not be portable to other CPUs)")
  target_compile_options(ka9q_compile_flags INTERFACE
    $<$<OR:$<C_COMPILER_ID:GNU>,$<C_COMPILER_ID:Clang>,$<C_COMPILER_ID:AppleClang>>:-march=native>
  )
else()
  message(STATUS "Native architecture optimization disabled (portable build)")
endif()

# ==================== PREPROCESSOR DEFINITIONS ====================
target_compile_definitions(ka9q_compile_flags INTERFACE
  _GNU_SOURCE=1
)

# ==================== MATH OPTIMIZATIONS ====================
# Compiler-specific unsafe math optimizations
# These are safe for DSP/radio applications but may break IEEE compliance

if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
  target_compile_options(ka9q_compile_flags INTERFACE
    -funsafe-math-optimizations
    -fno-math-errno
    -fcx-limited-range
    -freciprocal-math
    -fno-trapping-math
  )
  message(STATUS "Enabling GCC math optimizations")
  
elseif(CMAKE_C_COMPILER_ID MATCHES "Clang")
  target_compile_options(ka9q_compile_flags INTERFACE
    -fno-math-errno
    -freciprocal-math
    -fno-trapping-math
  )
  message(STATUS "Enabling Clang math optimizations")
endif()

# ==================== EXPORT TARGET ====================
# Make this target available to subdirectories
add_library(ka9q::compile_flags ALIAS ka9q_compile_flags)
