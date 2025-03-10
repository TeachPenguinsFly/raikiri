add_library(raikiri_simcore STATIC
  src/book.cpp
  src/engine.cpp
  src/mdgen.cpp
)
add_library(raikiri::simcore ALIAS raikiri_simcore)

target_include_directories(raikiri_simcore PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(raikiri_simcore PUBLIC raikiri::domain PRIVATE raikiri::warnings)
