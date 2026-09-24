add_library(raikiri_journal STATIC
  src/internal.cpp
  src/reader.cpp
  src/writer.cpp
)
add_library(raikiri::journal ALIAS raikiri_journal)

target_include_directories(raikiri_journal PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(raikiri_journal
  PUBLIC raikiri::common
  PRIVATE Crc32c::crc32c raikiri::warnings)
