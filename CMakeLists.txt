idf_component_register(SRCS MFRC522_I2C.cpp INCLUDE_DIRS . REQUIRES arduino-esp32)
target_compile_options(${COMPONENT_TARGET} PRIVATE -Wno-unused-but-set-variable)
