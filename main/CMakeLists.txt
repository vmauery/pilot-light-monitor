idf_component_register(SRCS "pilot-light-monitor.c" "https.c" "nanoprintf.c"
                    INCLUDE_DIRS "."
                    )
target_compile_options(${COMPONENT_LIB} PRIVATE "-Wno-format")
