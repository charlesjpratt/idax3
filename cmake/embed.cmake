# Turns a file into a C++ header of its bytes, so it can be built into the exe.
# Run as a script: cmake -DINPUT=<file> -DOUTPUT=<header> -DNAME=<array> -P embed.cmake

if(NOT INPUT OR NOT OUTPUT OR NOT NAME)
    message(FATAL_ERROR "embed.cmake needs INPUT, OUTPUT and NAME")
endif()

file(READ "${INPUT}" hex HEX)
string(LENGTH "${hex}" hex_len)
math(EXPR size "${hex_len} / 2")

# "0x..,": one per byte, broken into rows so the file is not a single line.
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],){24})" "\\1\n" bytes "${bytes}")

get_filename_component(source "${INPUT}" NAME)
file(WRITE "${OUTPUT}"
"// Generated from ${source} by cmake/embed.cmake; do not edit.
#pragma once
#include <cstddef>

inline const unsigned char ${NAME}[] = {
${bytes}
};
inline constexpr std::size_t ${NAME}_size = ${size};
")
