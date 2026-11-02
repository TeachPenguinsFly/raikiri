add_library(raikiri_portfolio STATIC
  src/pnl.cpp
  src/position.cpp
  src/position_book.cpp
  src/reconcile.cpp
  src/target.cpp
)
add_library(raikiri::portfolio ALIAS raikiri_portfolio)

target_include_directories(raikiri_portfolio PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(raikiri_portfolio
  PUBLIC raikiri::domain
  PRIVATE raikiri::warnings)

add_library(raikiri_portfolio_journal STATIC src/journal.cpp)
add_library(raikiri::portfolio_journal ALIAS raikiri_portfolio_journal)
target_link_libraries(raikiri_portfolio_journal
  PUBLIC raikiri::portfolio raikiri::journal raikiri::schema
  PRIVATE raikiri::warnings)
