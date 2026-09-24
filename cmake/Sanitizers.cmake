if(RAIKIRI_SANITIZE)
  add_compile_options(-fsanitize=${RAIKIRI_SANITIZE} -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=${RAIKIRI_SANITIZE})
endif()
