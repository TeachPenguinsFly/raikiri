include(GoogleTest)

function(raikiri_add_test name)
  cmake_parse_arguments(ARG "" "" "SRCS;LIBS" ${ARGN})
  add_executable(${name} ${ARG_SRCS})
  target_link_libraries(${name} PRIVATE ${ARG_LIBS} GTest::gtest_main raikiri::warnings)
  gtest_discover_tests(${name} DISCOVERY_TIMEOUT 30)
endfunction()

