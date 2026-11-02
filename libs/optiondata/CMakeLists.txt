# one target per layer, so an outward dependency fails to link

add_library(raikiri_optiondata STATIC
  src/board.cpp
  src/queries.cpp
)
add_library(raikiri::optiondata ALIAS raikiri_optiondata)
target_include_directories(raikiri_optiondata PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(raikiri_optiondata PUBLIC raikiri::options PRIVATE raikiri::warnings)

add_library(raikiri_optiondata_shm STATIC src/shm_market.cpp)
add_library(raikiri::optiondata_shm ALIAS raikiri_optiondata_shm)
target_link_libraries(raikiri_optiondata_shm
  PUBLIC raikiri::optiondata raikiri::admin
  PRIVATE raikiri::warnings)

add_library(raikiri_optiondata_http STATIC src/routes.cpp)
add_library(raikiri::optiondata_http ALIAS raikiri_optiondata_http)
target_link_libraries(raikiri_optiondata_http
  PUBLIC raikiri::optiondata raikiri::http
  PRIVATE glaze::glaze raikiri::warnings)
