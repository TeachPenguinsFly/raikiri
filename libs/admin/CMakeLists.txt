add_library(raikiri_admin STATIC
  src/assets.cpp
  src/assets_upstream.cpp
  src/desk.cpp
  src/format.cpp
  src/history.cpp
  src/margin.cpp
  src/registry.cpp
  src/routes.cpp
  src/stream.cpp
  src/stream_token.cpp
  src/trade_tape.cpp
  src/warm.cpp
)
add_library(raikiri::admin ALIAS raikiri_admin)

target_include_directories(raikiri_admin PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(raikiri_admin
  PUBLIC raikiri::domain raikiri::shm raikiri::http raikiri::config raikiri::books raikiri::candles raikiri::audit
  PRIVATE Boost::headers raikiri::net OpenSSL::Crypto glaze::glaze libpqxx::pqxx raikiri::warnings)
