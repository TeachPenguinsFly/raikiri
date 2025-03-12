add_library(raikiri_observ INTERFACE)
add_library(raikiri::observ ALIAS raikiri_observ)

target_include_directories(raikiri_observ INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
