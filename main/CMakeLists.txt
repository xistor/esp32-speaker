idf_component_register(SRCS "AudioI2s.cpp" "SpeakerApp.cpp" "main.cpp"
                    INCLUDE_DIRS ".")

target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++20 -fexceptions)