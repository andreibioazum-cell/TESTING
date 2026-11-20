# Converts binary SPIR-V file ${IN_SPV} into a C array ${VAR_NAME} in ${OUT_HEADER}
file(READ "${IN_SPV}" HEX_CONTENT HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," BYTE_LIST "${HEX_CONTENT}")
file(WRITE "${OUT_HEADER}" "#pragma once\n#include <stdint.h>\nalignas(4) static const uint8_t ${VAR_NAME}[] = {\n${BYTE_LIST}\n};\n")
