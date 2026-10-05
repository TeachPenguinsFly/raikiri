# one target per layer, so an outward dependency fails to link

add_library(raikiri_introspect STATIC
  src/stats.cpp
  src/latency.cpp
  src/queries.cpp
  src/clock_probe.cpp
  src/feed_window.cpp
  src/offset_bounds.cpp
)
add_library(raikiri::introspect ALIAS raikiri_introspect)
target_include_directories(raikiri_introspect PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(raikiri_introspect PUBLIC raikiri::observ PRIVATE raikiri::warnings)

add_library(raikiri_introspect_files STATIC src/files.cpp)
add_library(raikiri::introspect_files ALIAS raikiri_introspect_files)
target_link_libraries(raikiri_introspect_files
  PUBLIC raikiri::introspect
  PRIVATE glaze::glaze raikiri::warnings)

add_library(raikiri_introspect_probe STATIC src/server_time.cpp)
add_library(raikiri::introspect_probe ALIAS raikiri_introspect_probe)
target_link_libraries(raikiri_introspect_probe
  PUBLIC raikiri::introspect raikiri::exec
  PRIVATE raikiri::common raikiri::net raikiri::warnings)

add_library(raikiri_introspect_http STATIC src/routes.cpp src/metrics.cpp)
add_library(raikiri::introspect_http ALIAS raikiri_introspect_http)
target_link_libraries(raikiri_introspect_http
  PUBLIC raikiri::introspect raikiri::http
  PRIVATE glaze::glaze raikiri::warnings)
