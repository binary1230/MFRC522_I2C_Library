set(headers
    src/MFRC522_I2C.h
)

set(sources
        src/MFRC522_I2C.c
)

idf_component_register(
    INCLUDE_DIRS
        src
    SRCS
        ${headers}
        ${sources}
    REQUIRES
        driver
#        arduino-esp32
)

# target_compile_options(${COMPONENT_TARGET} PRIVATE -Wno-unused-but-set-variable)
