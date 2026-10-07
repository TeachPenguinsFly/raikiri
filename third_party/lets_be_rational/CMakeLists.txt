# built as shipped: no project warnings, and no fused multiply-add so mac and linux agree
add_library(raikiri_lets_be_rational STATIC
  erf_cody.cpp
  lets_be_rational.cpp
  normaldistribution.cpp
  rationalcubic.cpp
)
add_library(raikiri::lets_be_rational ALIAS raikiri_lets_be_rational)

target_include_directories(raikiri_lets_be_rational SYSTEM PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)
target_compile_definitions(raikiri_lets_be_rational PRIVATE NDEBUG NO_XL_API)
target_compile_options(raikiri_lets_be_rational PRIVATE -ffp-contract=off -w)
set_target_properties(raikiri_lets_be_rational PROPERTIES DISABLE_PRECOMPILE_HEADERS ON)
