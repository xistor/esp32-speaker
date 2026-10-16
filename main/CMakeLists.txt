file(GLOB_RECURSE SOURCES
    "*.c"
    "*.cpp"
    "audio/*.cpp"
    "display/*.cpp"
    "res/**/*.c"
    "ui/*.cpp"
    "common/*.cpp"
    "network/*.cpp"
    "hap/*.cpp"
)
idf_component_register(SRCS ${SOURCES}
                    INCLUDE_DIRS "." "audio" "display" "common" "ui" "network" "hap")

target_compile_options(${COMPONENT_LIB} PRIVATE 
                    $<$<COMPILE_LANGUAGE:CXX>:
                        -std=gnu++20
                        -fexceptions
                        -Wno-error=missing-field-initializers
                        -Wno-reorder
                        >
                    $<$<COMPILE_LANGUAGE:C>:
                        >)