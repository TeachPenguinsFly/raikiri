add_library(raikiri_strategies INTERFACE)
add_library(raikiri::strategies ALIAS raikiri_strategies)

target_include_directories(raikiri_strategies INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(raikiri_strategies INTERFACE raikiri::strategy)
