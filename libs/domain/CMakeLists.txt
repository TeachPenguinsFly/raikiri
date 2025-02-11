add_library(raikiri_domain STATIC
  src/identifiers.cpp
  src/instrument.cpp
)
add_library(raikiri::domain ALIAS raikiri_domain)

target_include_directories(raikiri_domain PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(raikiri_domain PUBLIC raikiri::common PRIVATE raikiri::warnings)
