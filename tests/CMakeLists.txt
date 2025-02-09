include(GoogleTest)

function(raikiri_add_test name)
  cmake_parse_arguments(ARG "" "" "SRCS;LIBS" ${ARGN})
  add_executable(${name} ${ARG_SRCS})
  target_link_libraries(${name} PRIVATE ${ARG_LIBS} GTest::gtest_main raikiri::warnings)
  gtest_discover_tests(${name} DISCOVERY_TIMEOUT 30)
endfunction()

raikiri_add_test(fixed_point_test SRCS common/fixed_point_test.cpp LIBS raikiri::common)

raikiri_add_test(fixed_string_test SRCS common/fixed_string_test.cpp LIBS raikiri::common)

raikiri_add_test(ulid_test SRCS common/ulid_test.cpp LIBS raikiri::common)

raikiri_add_test(platform_test SRCS common/platform_test.cpp LIBS raikiri::common)

raikiri_add_test(identifiers_test SRCS domain/identifiers_test.cpp LIBS raikiri::domain)
