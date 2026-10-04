# one target per layer, so an outward dependency fails to link

add_library(raikiri_joe STATIC
  src/order.cpp
  src/sor.cpp
  src/schedule.cpp
  src/parent.cpp
  src/desk.cpp
)
add_library(raikiri::joe ALIAS raikiri_joe)
target_include_directories(raikiri_joe PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(raikiri_joe PUBLIC raikiri::domain PRIVATE raikiri::warnings)

add_library(raikiri_joe_infra STATIC src/shm_market.cpp src/runner.cpp)
add_library(raikiri::joe_infra ALIAS raikiri_joe_infra)
target_link_libraries(raikiri_joe_infra
  PUBLIC raikiri::joe raikiri::shm raikiri::strategy
  PRIVATE raikiri::exec raikiri::warnings)

add_library(raikiri_joe_http STATIC src/routes.cpp)
add_library(raikiri::joe_http ALIAS raikiri_joe_http)
target_link_libraries(raikiri_joe_http
  PUBLIC raikiri::joe raikiri::http
  PRIVATE glaze::glaze raikiri::warnings)
