# Turns a binary file into a C array: cmake -DIN=... -DOUT=... -DNAME=... -P embed.cmake
file(READ "${IN}" hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," hex "${hex}")
file(WRITE "${OUT}" "static const unsigned char ${NAME}[] = {${hex}};\n")
