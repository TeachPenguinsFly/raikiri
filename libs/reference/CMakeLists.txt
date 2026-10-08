# one target per layer, so an outward dependency fails to link

add_library(raikiri_reference STATIC
  src/mid.cpp
  src/engine.cpp
  src/layout.cpp
)
add_library(raikiri::reference ALIAS raikiri_reference)
target_include_directories(raikiri_reference PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(raikiri_reference PUBLIC raikiri::common raikiri::domain PRIVATE raikiri::warnings)

add_library(raikiri_reference_shm STATIC src/segment.cpp src/gateways.cpp)
add_library(raikiri::reference_shm ALIAS raikiri_reference_shm)
target_link_libraries(raikiri_reference_shm
  PUBLIC raikiri::reference raikiri::shm
  PRIVATE raikiri::common raikiri::warnings)

add_library(raikiri_reference_http STATIC src/routes.cpp)
add_library(raikiri::reference_http ALIAS raikiri_reference_http)
target_link_libraries(raikiri_reference_http
  PUBLIC raikiri::reference raikiri::http
  PRIVATE glaze::glaze raikiri::warnings)
