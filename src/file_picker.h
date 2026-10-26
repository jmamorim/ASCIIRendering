// File picker. SDL may call back on another thread, so the main loop picks
// the result up with takePickedFile().
#pragma once

#include <string>

#ifdef __EMSCRIPTEN__
extern "C" void PickImageFile(); // EM_JS functions have C linkage
#else
void PickImageFile();
#endif

// The file picked since the last call, or "" if none
std::string takePickedFile();
