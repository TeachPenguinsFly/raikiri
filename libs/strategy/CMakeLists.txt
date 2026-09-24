# the extension point, factory and registry, with no http
add_library(raikiri_strategy STATIC
  src/factory.cpp
  src/registry.cpp
)
add_library(raikiri::strategy ALIAS raikiri_strategy)

target_include_directories(raikiri_strategy PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(raikiri_strategy
  PUBLIC raikiri::domain raikiri::portfolio
  PRIVATE raikiri::warnings)

# kept apart so linking a strategy does not link a server
add_library(raikiri_strategy_control STATIC src/control.cpp)
add_library(raikiri::strategy_control ALIAS raikiri_strategy_control)

target_link_libraries(raikiri_strategy_control
  PUBLIC raikiri::http raikiri::strategy
  PRIVATE glaze::glaze raikiri::warnings)
