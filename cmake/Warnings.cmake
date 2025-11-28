# raikiri::warnings — interface target attached to every first-party target.
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

# -Wnull-dereference IS DELIBERATELY ABSENT, and the reason is about gcc rather
# than about the check. gcc raises it from the OPTIMIZER, after inlining, and a
# warning produced there has already lost the system-header attribution that
# -isystem gives Boost — so -Werror stopped the first Linux build on
# boost/asio/detail/impl/scheduler.ipp:340 in eight translation units across
# libs/net, libs/admin, libs/strategy and three venue adapters, none of which
# can fix the line. Per-file exemptions were tried and are the wrong shape: a
# new file that touches asio inherits the failure and the next person pays for
# it again.
#
# WHAT ACTUALLY CATCHES A NULL DEREFERENCE HERE is the dev-asan preset, where
# UBSan's null check runs on real execution rather than on the optimizer's
# guess, and it runs on every test in CI. Clang never raised this warning at
# all, which is why a tree built only on a mac carried it for months.

if(RAIKIRI_WERROR)
  target_compile_options(raikiri_warnings INTERFACE -Werror)
endif()
