add_library(raikiri_warnings INTERFACE)
add_library(raikiri::warnings ALIAS raikiri_warnings)

target_compile_options(raikiri_warnings INTERFACE
  -Wall
  -Wextra
  -Wpedantic
  -Wshadow
  -Wconversion
  -Wsign-conversion
  -Wnon-virtual-dtor
  -Wold-style-cast
  -Woverloaded-virtual
  -Wcast-align
  -Wdouble-promotion
  -Wimplicit-fallthrough
)

# no -Wnull-dereference: gcc raises it inside boost asio headers; ubsan covers null checks

# gcc 12-14 at -O2 report std::string's inlined copies as out of bounds (gcc bug 105329); asan covers bounds
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  target_compile_options(raikiri_warnings INTERFACE -Wno-error=array-bounds)
endif()

if(RAIKIRI_WERROR)
  target_compile_options(raikiri_warnings INTERFACE -Werror)
endif()
