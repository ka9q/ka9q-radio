# cmake/AddDrivers.cmake
# Helper function for building SDR driver modules
# NOTE: This is now deprecated in favor of kr_add_driver() defined in src/CMakeLists.txt
# Keeping this file for backwards compatibility, but consider removing it.

include_guard(GLOBAL)

function(add_driver name)
  add_library(${name}_drv MODULE ${ARGN})
  set_target_properties(${name}_drv PROPERTIES OUTPUT_NAME ${name})
  target_link_libraries(${name}_drv PRIVATE radio)
  install(TARGETS ${name}_drv LIBRARY DESTINATION lib/ka9q-radio)
endfunction()
