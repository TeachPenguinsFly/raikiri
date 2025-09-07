add_library(raikiri_strategy INTERFACE)
add_library(raikiri::strategy ALIAS raikiri_strategy)

target_include_directories(raikiri_strategy INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(raikiri_strategy
  INTERFACE raikiri::domain raikiri::portfolio)
