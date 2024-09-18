set(headers
    src/espasyncota.h
)

set(sources
    src/espasyncota.cpp
)

set(dependencies
    esp_https_ota
    esp_partition

    cpputils
    espchrono
    espcpputils
    esphttpdutils
)

idf_component_register(
    INCLUDE_DIRS
        src
    SRCS
        ${headers}
        ${sources}
    REQUIRES
        ${dependencies}
)

set_property(TARGET ${COMPONENT_LIB} PROPERTY CXX_STANDARD 23)

target_compile_options(${COMPONENT_TARGET}
    PRIVATE
        -fstack-reuse=all
        -fstack-protector-all
        -Wno-unused-function
        -Wno-deprecated-declarations
        -Wno-missing-field-initializers
        -Wno-parentheses
)
