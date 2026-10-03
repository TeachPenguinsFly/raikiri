add_library(raikiri_options STATIC
  src/chain.cpp
  src/convention.cpp
  src/key.cpp
)
add_library(raikiri::options ALIAS raikiri_options)

target_include_directories(raikiri_options PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(raikiri_options
  PUBLIC raikiri::domain
  PRIVATE raikiri::warnings)
