# one target per layer, so an outward dependency fails to link

add_library(raikiri_optionrisk STATIC
  src/risk.cpp
  src/queries.cpp
)
add_library(raikiri::optionrisk ALIAS raikiri_optionrisk)
target_include_directories(raikiri_optionrisk PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(raikiri_optionrisk PUBLIC raikiri::optiondata PRIVATE raikiri::warnings)

add_library(raikiri_optionrisk_infra STATIC src/shm_holdings.cpp src/history_realized.cpp src/file_ledger.cpp)
add_library(raikiri::optionrisk_infra ALIAS raikiri_optionrisk_infra)
target_link_libraries(raikiri_optionrisk_infra
  PUBLIC raikiri::optionrisk raikiri::admin
  PRIVATE glaze::glaze raikiri::log raikiri::warnings)

add_library(raikiri_optionrisk_http STATIC src/routes.cpp)
add_library(raikiri::optionrisk_http ALIAS raikiri_optionrisk_http)
target_link_libraries(raikiri_optionrisk_http
  PUBLIC raikiri::optionrisk raikiri::http
  PRIVATE glaze::glaze raikiri::warnings)
