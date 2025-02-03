# Global sanitizer flags, driven by the RAIKIRI_SANITIZE cache variable
# (set per preset: dev-asan = "address,undefined", dev-tsan = "thread").
# Applied globally so every first-party object and link line agrees; prebuilt
# Conan dependencies stay uninstrumented, which sanitizer runtimes tolerate.
if(RAIKIRI_SANITIZE)
  add_compile_options(-fsanitize=${RAIKIRI_SANITIZE} -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=${RAIKIRI_SANITIZE})
endif()
