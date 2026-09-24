# static so the service can call the registration symbol; see register.hpp
add_library(raikiri_strategies STATIC src/register.cpp)
add_library(raikiri::strategies ALIAS raikiri_strategies)

target_include_directories(raikiri_strategies PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(raikiri_strategies
  PUBLIC raikiri::strategy
  PRIVATE raikiri::warnings)
