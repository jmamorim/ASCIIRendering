// The window's contents: the left panel (buttons, sliders, preview) and the ASCII output
#pragma once

#include "imgui.h"

// Rebuild what depends on the sliders and convert the image again
void onParamsChanged();

void drawUi(float dpi, ImFont* font);
