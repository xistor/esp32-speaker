file(GLOB_RECURSE SOURCES
    "*.cpp"
    "audio/*.cpp"
    "display/*.cpp"
    "res/**/*.c"
    "ui/*.cpp"
    "common/*.cpp"
    "network/*.cpp"
)
idf_component_register(SRCS ${SOURCES}
                    INCLUDE_DIRS "." "audio" "display" "common" "ui" "network")

target_compile_options(${COMPONENT_LIB} PRIVATE 
                    $<$<COMPILE_LANGUAGE:CXX>:
                        -std=gnu++20
                        -fexceptions
                        -Wno-error=missing-field-initializers
                        -Wno-reorder
                        >
                    $<$<COMPILE_LANGUAGE:C>:
                        >)