add_library(raikiri_options STATIC
  src/chain.cpp
  src/convention.cpp
  src/forward.cpp
  src/key.cpp
  src/pricing.cpp
  src/surface.cpp
)
add_library(raikiri::options ALIAS raikiri_options)

# gcc fuses multiply-adds by default even under -std=c++23, and arm64 has fma where plain x86-64 does not
set_source_files_properties(src/pricing.cpp src/surface.cpp
  PROPERTIES COMPILE_OPTIONS -ffp-contract=off SKIP_PRECOMPILE_HEADERS ON)

target_include_directories(raikiri_options PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(raikiri_options
  PUBLIC raikiri::domain
  PRIVATE raikiri::lets_be_rational raikiri::warnings)
