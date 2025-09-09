# Generated code lands in the build tree only, never checked in — that is
# what keeps schema/journal.xml and the codecs from drifting apart.
find_program(SBEPPC_EXECUTABLE sbeppc REQUIRED)

set(RAIKIRI_SBE_SCHEMA ${PROJECT_SOURCE_DIR}/schema/journal.xml)
set(RAIKIRI_SBE_OUT ${CMAKE_CURRENT_BINARY_DIR}/generated)

add_custom_command(
  OUTPUT ${RAIKIRI_SBE_OUT}/raikiri_journal/raikiri_journal.hpp
  COMMAND ${SBEPPC_EXECUTABLE} --schema-name raikiri_journal
          --output-dir ${RAIKIRI_SBE_OUT} ${RAIKIRI_SBE_SCHEMA}
  DEPENDS ${RAIKIRI_SBE_SCHEMA}
  COMMENT "sbeppc: generating journal codecs"
  VERBATIM)

add_custom_target(raikiri_schema_gen
  DEPENDS ${RAIKIRI_SBE_OUT}/raikiri_journal/raikiri_journal.hpp)

add_library(raikiri_schema INTERFACE)
add_library(raikiri::schema ALIAS raikiri_schema)
add_dependencies(raikiri_schema raikiri_schema_gen)

target_include_directories(raikiri_schema INTERFACE
  ${RAIKIRI_SBE_OUT}
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(raikiri_schema
  INTERFACE raikiri::journal sbepp::sbepp)
