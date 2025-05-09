add_library(raikiri_book STATIC
  src/ladder.cpp
)
add_library(raikiri::book ALIAS raikiri_book)

target_include_directories(raikiri_book PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(raikiri_book PUBLIC raikiri::domain PRIVATE raikiri::warnings)
