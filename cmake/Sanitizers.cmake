# Global sanitizer flags, driven by the RAIKIRI_SANITIZE cache variable
# (set per preset: dev-asan = "address,undefined", dev-tsan = "thread").
# Applied globally so every first-party object and link line agrees; prebuilt
# Conan dependencies stay uninstrumented, which sanitizer runtimes tolerate.
if(RAIKIRI_SANITIZE)
  add_compile_options(-fsanitize=${RAIKIRI_SANITIZE} -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=${RAIKIRI_SANITIZE})
endif()

# Coverage instrumentation for fuzzing has to apply to every first-party
# object, not just the fuzz target's own file: libFuzzer steers by coverage
# feedback, and a parser compiled without instrumentation is invisible to it
# (the symptom is a fuzzer that runs at millions of execs/sec and never grows
# its corpus). Linking the runtime stays with the fuzz targets themselves.
if(RAIKIRI_BUILD_FUZZ)
  add_compile_options(-fsanitize=fuzzer-no-link)
endif()
