add_library(raikiri_http STATIC
  src/fetch.cpp
  src/problem.cpp
  src/server.cpp
)
add_library(raikiri::http ALIAS raikiri_http)

target_include_directories(raikiri_http PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
# beast stays private; no boost type reaches the public header
target_link_libraries(raikiri_http
  PUBLIC raikiri::net
  PRIVATE Boost::headers glaze::glaze raikiri::warnings)
