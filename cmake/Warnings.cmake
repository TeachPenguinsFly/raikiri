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

if(RAIKIRI_WERROR)
  target_compile_options(raikiri_warnings INTERFACE -Werror)
endif()
