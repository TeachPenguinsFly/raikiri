# Precompiled headers, compiled a fixed three times and reused by every
# first-party target.
#
# The headers named in them are the ones a translation unit pays for whether
# or not it uses them. Measured on this tree with -fsyntax-only: <simdjson.h>
# costs 0.62s, <gtest/gtest.h> 0.46s, and the libc++ headers between them
# another 0.2s or so — against 0.02s for an empty TU.
#
# REUSE_FROM is what makes this affordable: the PCH is built three times no
# matter how many targets consume it. One per target would cost more than it
# saves across 120 targets.
#
# WARN: a PCH hides a missing #include — a TU that compiles only because the
# PCH named <vector> for it breaks the moment RAIKIRI_PCH is OFF. That switch
# exists so the tree can be checked honestly; a build that passes only with
# the PCH on is a broken build, not a fast one.
#
# INVARIANT: simdjson may only be precompiled into a target that links it.
# The amalgamation defines functions inline, so a TU that parses it emits
# references to simdjson symbols whether or not the source names one, and a
# target without the library then fails to link. Hence the LINK_LIBRARIES
# test below rather than a directory list: libs/venues/sim links no parser.
#
# gtest is deliberately kept out of the non-test PCHs. Its macros would
# otherwise be visible to production code, and code that compiles because a
# test framework happened to be in scope is worse than code that is slow.

function(_raikiri_pch_targets dir out)
  get_property(targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
  get_property(subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
  foreach(sub IN LISTS subdirs)
    _raikiri_pch_targets("${sub}" sub_targets)
    list(APPEND targets ${sub_targets})
  endforeach()
  set(${out} "${targets}" PARENT_SCOPE)
endfunction()

# ANCHORED TO RAIKIRI, NOT TO THE BUILD'S ROOT.
#
# `CMAKE_SOURCE_DIR` is the top of whatever build is running, and raikiri is
# meant to be added as a subproject by a repository that keeps its own code
# private. Under such a build every path below resolved into the OTHER
# repository, and the PCH failed on a header that was never there — which is
# the framework breaking exactly the consumer it exists for.
set(RAIKIRI_PCH_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")

function(raikiri_enable_pch)
  set(stub "${CMAKE_BINARY_DIR}/raikiri_pch.cpp")
  file(WRITE "${stub}" "// generated: carrier TU for the shared precompiled headers\n")

  add_library(raikiri_pch STATIC "${stub}")
  target_precompile_headers(raikiri_pch PRIVATE "${RAIKIRI_PCH_ROOT}/cmake/pch_std.hpp")

  add_library(raikiri_pch_json STATIC "${stub}")
  target_link_libraries(raikiri_pch_json PRIVATE simdjson::simdjson)
  target_precompile_headers(raikiri_pch_json PRIVATE "${RAIKIRI_PCH_ROOT}/cmake/pch_json.hpp")

  add_library(raikiri_pch_tests STATIC "${stub}")
  target_link_libraries(raikiri_pch_tests PRIVATE GTest::gtest)
  target_precompile_headers(raikiri_pch_tests PRIVATE "${RAIKIRI_PCH_ROOT}/cmake/pch_tests.hpp")

  set(carriers raikiri_pch raikiri_pch_json raikiri_pch_tests)
  _raikiri_pch_targets("${RAIKIRI_PCH_ROOT}" all_targets)
  foreach(t IN LISTS all_targets)
    if(t IN_LIST carriers)
      continue()
    endif()
    get_target_property(type ${t} TYPE)
    if(type STREQUAL "INTERFACE_LIBRARY" OR type STREQUAL "UTILITY")
      continue()
    endif()
    get_target_property(src_dir ${t} SOURCE_DIR)
    get_target_property(links ${t} LINK_LIBRARIES)
    if(src_dir MATCHES "/tests")
      target_precompile_headers(${t} REUSE_FROM raikiri_pch_tests)
    elseif(links MATCHES "simdjson")
      target_precompile_headers(${t} REUSE_FROM raikiri_pch_json)
    else()
      target_precompile_headers(${t} REUSE_FROM raikiri_pch)
    endif()
  endforeach()
endfunction()
