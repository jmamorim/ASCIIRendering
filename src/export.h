// Getting the ASCII output out: the clipboard, image files and video files
#pragma once

#include "imgui.h"

#include <vector>

extern ImVec2 g_asciiMin, g_asciiMax; // where paintAscii drew the characters, in window points

void copyToClipboard();

// Screenshot of the ASCII area in the frame just drawn, so call it before the swap.
// Gives opaque RGBA, scaled by the export scale, bottom row first (as GL reads it).
bool readAsciiPixels(std::vector<unsigned char>& pixels, int& w, int& h);

void exportAsciiImage();

// The export popups; open them with ImGui::OpenPopup("Export image") / ("Export video")
void showExportPopup();

// Call after the frame is drawn, before the swap: takes the screenshots exports asked for
void runPendingExport();

// Video export: every frame of the GIF or video, in order
bool videoExportRunning();
// Call before drawing, instead of updatePlayback when it returns true: shows the next frame
bool exportVideoFrame();
// Progress bar and Cancel button, drawn in place of the export buttons
void showVideoExportProgress();
