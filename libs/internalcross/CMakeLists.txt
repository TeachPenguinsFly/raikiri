add_library(raikiri_internalcross STATIC src/match.cpp src/settings.cpp src/liability.cpp src/desk.cpp src/control.cpp)
add_library(raikiri::internalcross ALIAS raikiri_internalcross)
target_include_directories(raikiri_internalcross PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(raikiri_internalcross
  PUBLIC raikiri::audit raikiri::oms raikiri::oms_trail raikiri::portfolio
  PRIVATE fmt::fmt raikiri::warnings)

add_library(raikiri_internalcross_infra STATIC src/journal_log.cpp src/file_store.cpp src/shm_touches.cpp)
add_library(raikiri::internalcross_infra ALIAS raikiri_internalcross_infra)
target_link_libraries(raikiri_internalcross_infra
  PUBLIC raikiri::internalcross raikiri::journal raikiri::oms_journal raikiri::portfolio_journal raikiri::shm
  PRIVATE raikiri::exec glaze::glaze raikiri::warnings)

add_library(raikiri_internalcross_http STATIC src/routes.cpp)
add_library(raikiri::internalcross_http ALIAS raikiri_internalcross_http)
target_link_libraries(raikiri_internalcross_http
  PUBLIC raikiri::internalcross raikiri::http
  PRIVATE glaze::glaze raikiri::warnings)
