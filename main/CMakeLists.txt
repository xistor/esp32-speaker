file(GLOB_RECURSE SOURCES
    "*.cpp"
    "audio/*.cpp"
    "display/*.cpp"
    "fonts/*.c"
)
idf_component_register(SRCS ${SOURCES}
                    INCLUDE_DIRS "." "audio" "display" "common")

target_compile_options(${COMPONENT_LIB} PRIVATE 
                    $<$<COMPILE_LANGUAGE:CXX>:
                        -std=gnu++20
                        -fexceptions
                        -Wno-error=missing-field-initializers
                        -Wno-reorder
                        >
                    $<$<COMPILE_LANGUAGE:C>:
                        >)