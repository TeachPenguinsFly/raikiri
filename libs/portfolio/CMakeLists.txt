add_library(raikiri_portfolio STATIC
  src/pnl.cpp
  src/position.cpp
  src/position_book.cpp
)
add_library(raikiri::portfolio ALIAS raikiri_portfolio)

target_include_directories(raikiri_portfolio PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(raikiri_portfolio PUBLIC raikiri::domain PRIVATE raikiri::warnings)
