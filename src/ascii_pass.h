// GPU pass: ascii.frag runs once per cell into a cols x rows target and
// writes the cell's 6 circle values; the glyph lookup stays on the CPU.
#pragma once

#include "gl.h"
#include "glyphs.h"

#include <string>
#include <cstdint>
#include <vector>

struct Cell {Vec v; float strength, angle; uint32_t color;};

extern std::vector<std::string> g_lines; // current ASCII output
extern std::vector<std::vector<uint32_t>> g_colors; 

bool initAsciiPass();
std::vector<Cell> runPass(GLuint tex, int cols, int rows, float cw, float ch, int step, float exponent);

// Image -> lines of ASCII, using the current slider values
void renderAscii();
