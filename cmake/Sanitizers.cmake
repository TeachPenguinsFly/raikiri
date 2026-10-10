# ci takes -g1: line tables are enough for sanitizer stacks and full debug info outgrows the runner's disk
set(RAIKIRI_SANITIZE_DEBUG "-g" CACHE STRING "debug info flag for sanitizer builds")

if(RAIKIRI_SANITIZE)
  add_compile_options(-fsanitize=${RAIKIRI_SANITIZE} -fno-omit-frame-pointer ${RAIKIRI_SANITIZE_DEBUG})
  add_link_options(-fsanitize=${RAIKIRI_SANITIZE})
endif()
