add_library(raikiri_config STATIC
  src/credentials.cpp
  src/one_password.cpp
  src/risk.cpp
  src/secret.cpp
  src/settings.cpp
)
add_library(raikiri::config ALIAS raikiri_config)

target_include_directories(raikiri_config PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
# openssl stays out of public headers
target_link_libraries(raikiri_config
  PUBLIC raikiri::domain raikiri::oms
  PRIVATE glaze::glaze OpenSSL::Crypto raikiri::warnings)
