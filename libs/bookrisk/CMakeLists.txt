# one target per layer, so an outward dependency fails to link

add_library(raikiri_bookrisk STATIC
  src/limits.cpp
  src/track.cpp
  src/value.cpp
  src/desk.cpp
)
add_library(raikiri::bookrisk ALIAS raikiri_bookrisk)
target_include_directories(raikiri_bookrisk PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(raikiri_bookrisk
  PUBLIC raikiri::audit raikiri::oms raikiri::portfolio
  PRIVATE fmt::fmt raikiri::warnings)

add_library(raikiri_bookrisk_infra STATIC src/file_store.cpp src/notifiers.cpp src/shm_balances.cpp)
add_library(raikiri::bookrisk_infra ALIAS raikiri_bookrisk_infra)
target_link_libraries(raikiri_bookrisk_infra
  PUBLIC raikiri::bookrisk raikiri::config raikiri::shm
  PRIVATE glaze::glaze raikiri::log raikiri::net raikiri::warnings)

add_library(raikiri_bookrisk_http STATIC src/routes.cpp)
add_library(raikiri::bookrisk_http ALIAS raikiri_bookrisk_http)
target_link_libraries(raikiri_bookrisk_http
  PUBLIC raikiri::bookrisk raikiri::http
  PRIVATE glaze::glaze raikiri::warnings)
