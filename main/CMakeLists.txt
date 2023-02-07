idf_component_register(SRCS "pilot-light-monitor.c" "https.c"
                            "base64.c" "nanoprintf.c"
                    INCLUDE_DIRS "."
                    )
target_compile_options(${COMPONENT_LIB} PRIVATE "-Wno-format")
