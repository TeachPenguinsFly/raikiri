add_library(raikiri_config STATIC
  src/secret.cpp
)
add_library(raikiri::config ALIAS raikiri_config)

target_include_directories(raikiri_config PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
# OpenSSL is an implementation detail (fingerprint digest only) — it must not
# appear in any public header.
target_link_libraries(raikiri_config
  PUBLIC raikiri::domain
  PRIVATE OpenSSL::Crypto raikiri::warnings)
