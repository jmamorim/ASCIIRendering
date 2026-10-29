// Parameters shown as sliders
#pragma once

enum { P_COLS, P_STEP, P_CONTRAST, P_RADIUS, P_STAGGER, P_FONT, P_EDGE, P_COUNT };

struct Param
{
    const char* name;
    int minv, maxv, init; // slider range (integers)
    float scale;          // real value = slider position * scale
    int decimals;         // decimals to show in the label
    int pos;              // current slider position
};

extern Param P[P_COUNT];

float val(int i);
