add_library(raikiri_journal STATIC
  src/detail.cpp
  src/writer.cpp
)
add_library(raikiri::journal ALIAS raikiri_journal)

target_include_directories(raikiri_journal PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
# crc32c is an implementation detail (src/ only), never in public headers.
target_link_libraries(raikiri_journal
  PUBLIC raikiri::common
  PRIVATE Crc32c::crc32c raikiri::warnings)
