# AppleClang ships no libFuzzer runtime, so these are off by default and a
# plain macOS build must not fail for want of them. The `fuzz` preset selects
# Homebrew's clang; CI runs them on Linux.
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(STATUS "fuzz: skipped (needs Clang with libFuzzer)")
  return()
endif()

include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_FLAGS "-fsanitize=fuzzer")
check_cxx_source_compiles(
  "#include <cstdint>\n#include <cstddef>\nextern \"C\" int LLVMFuzzerTestOneInput(const uint8_t*, size_t){return 0;}"
  RAIKIRI_HAVE_LIBFUZZER)
unset(CMAKE_REQUIRED_FLAGS)

if(NOT RAIKIRI_HAVE_LIBFUZZER)
  message(STATUS "fuzz: skipped (compiler accepts -fsanitize=fuzzer but the runtime is missing)")
  return()
endif()

function(raikiri_add_fuzzer name)
  cmake_parse_arguments(ARG "" "SRC;CORPUS" "LIBS" ${ARGN})
  add_executable(${name} ${ARG_SRC})
  # Sanitizers come from the preset; fuzzer instrumentation is added on top
  # so a corpus found under ASan reproduces under ASan.
  target_compile_options(${name} PRIVATE -fsanitize=fuzzer-no-link)
  target_link_options(${name} PRIVATE -fsanitize=fuzzer)
  target_link_libraries(${name}
    PRIVATE ${ARG_LIBS} raikiri::warnings)
  if(ARG_CORPUS)
    set_property(TARGET ${name} PROPERTY RAIKIRI_CORPUS ${ARG_CORPUS})
  endif()
endfunction()

raikiri_add_fuzzer(bybit_wire_fuzz SRC bybit_wire_fuzz.cpp LIBS raikiri::venue_bybit
                   CORPUS ${CMAKE_CURRENT_SOURCE_DIR}/corpus/bybit)
raikiri_add_fuzzer(journal_frame_fuzz SRC journal_frame_fuzz.cpp LIBS raikiri::journal)
raikiri_add_fuzzer(decimal_fuzz SRC decimal_fuzz.cpp LIBS raikiri::common)

# Seeds the bybit corpus from the same captured frames the unit tests use,
# each prefixed with the selector byte that routes it to its parser.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_custom_target(fuzz_corpus
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/seed_corpus.py
          ${PROJECT_SOURCE_DIR}/tests/venues/bybit/fixtures
          ${CMAKE_CURRENT_SOURCE_DIR}/corpus/bybit
  COMMENT "seeding fuzz corpus from test fixtures")
