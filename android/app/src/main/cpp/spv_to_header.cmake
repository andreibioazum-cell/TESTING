# Converts binary SPIR-V file ${INPUT_SPV} into a C array ${VAR_NAME} in ${OUTPUT_HDR}
file(READ "${INPUT_SPV}" HEX_CONTENT HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," BYTE_LIST "${HEX_CONTENT}")
file(WRITE "${OUTPUT_HDR}" "#pragma once\n#include <stdint.h>\nalignas(4) static const uint8_t ${VAR_NAME}[] = {\n${BYTE_LIST}\n};\n")
