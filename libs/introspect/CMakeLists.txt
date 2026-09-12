# One target per layer, so a dependency pointing outward fails to link.

add_library(raikiri_introspect STATIC
  src/stats.cpp
  src/queries.cpp
)
add_library(raikiri::introspect ALIAS raikiri_introspect)
target_include_directories(raikiri_introspect PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(raikiri_introspect PRIVATE raikiri::warnings)

add_library(raikiri_introspect_files STATIC src/files.cpp)
add_library(raikiri::introspect_files ALIAS raikiri_introspect_files)
target_link_libraries(raikiri_introspect_files
  PUBLIC raikiri::introspect
  PRIVATE glaze::glaze raikiri::warnings)

add_library(raikiri_introspect_http STATIC src/routes.cpp)
add_library(raikiri::introspect_http ALIAS raikiri_introspect_http)
target_link_libraries(raikiri_introspect_http
  PUBLIC raikiri::introspect raikiri::http
  PRIVATE glaze::glaze raikiri::warnings)
