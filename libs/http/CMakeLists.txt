add_library(raikiri_http STATIC
  src/fetch.cpp
  src/server.cpp
)
add_library(raikiri::http ALIAS raikiri_http)

target_include_directories(raikiri_http PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
# Beast is an implementation detail: no Boost type reaches the public header,
# so a consumer of this library does not inherit the dependency.
target_link_libraries(raikiri_http
  PRIVATE Boost::headers raikiri::warnings)
