# (AI-assisted)
# cmake -DIN=<file> -DOUT=<file.c> -DNAME=<symbol> -P bin2c.cmake
file(READ ${IN} HEX HEX)
string(LENGTH "${HEX}" LEN)
math(EXPR SIZE "${LEN} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," BYTES "${HEX}")
file(WRITE ${OUT} "// generated from ${IN}\n#include <stdint.h>\n#include <stddef.h>\n")
file(APPEND ${OUT} "const uint8_t ${NAME}[] __attribute__((aligned(4))) = {${BYTES}};\n")
file(APPEND ${OUT} "const size_t ${NAME}_size = ${SIZE};\n")
