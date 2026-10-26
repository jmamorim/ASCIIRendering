#include "params.h"

Param P[P_COUNT] = {
    { "Columns",         20, 250, 100, 1.00f, 0 },
    { "Sample step",      1,   8,   2, 1.00f, 0 },
    { "Contrast",        10,  40,  10, 0.10f, 1 },
    { "Circle radius",   15,  45,  30, 0.01f, 2 },
    { "Stagger",          0,  10,   4, 0.01f, 2 },
    { "Glyph font size", 50, 100,  80, 0.01f, 2 },
};

float val(int i)
{
    return (float)P[i].pos * P[i].scale;
}
