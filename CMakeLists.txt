set(headers
    src/espasyncota.h
)

set(sources
    src/espasyncota.cpp
)

set(dependencies
    esp_https_ota

    expected
    fmt

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

target_compile_options(${COMPONENT_TARGET}
    PRIVATE
        -fstack-reuse=all
        -fstack-protector-all
        -Wno-unused-function
        -Wno-deprecated-declarations
        -Wno-missing-field-initializers
        -Wno-parentheses
)
