// Media: a still image, a GIF or a video. Animated media is converted one
// frame at a time with the same code as a still image (showFrame).
#pragma once

#include "gl.h"

#include <string>
#include <vector>

enum MediaKind { M_NONE, M_IMAGE, M_GIF, M_VIDEO };

extern MediaKind g_kind;
extern bool g_playing;
extern double g_time;      // playback position, seconds
extern double g_duration;  // 0 = unknown
extern int g_frameShown;   // index of the frame in g_image / g_lines

extern GLuint g_image;     // the preview, also the shader's input
extern int g_mediaW, g_mediaH;
extern bool g_imageReady;

// GIF: every frame is decoded up front
extern std::vector<unsigned char> g_gifPixels; // RGBA, frame after frame
extern std::vector<double> g_gifStart;         // start time of each frame, seconds

void createPreview(int w, int h);
void showFrame(const unsigned char* rgba);
void startMedia(MediaKind kind, double duration);
void openFile(const std::string& path);

// Move the playback clock and convert the frame it lands on, once per loop
void updatePlayback();

// Video. Desktop: video_desktop.cpp (ffmpeg). Browser: video_web.cpp (<video> element).
extern const char* OPEN_ERROR;
bool loadVideo(const std::string& path);
bool startVideo(double t); // seek to `t` seconds
void stopVideo();
void updateVideo(double dt); // updatePlayback's part for a video

// For the video export: after startVideo(0), step through the frames in order,
// ignoring the playback clock
double videoFps();
const char* videoRate(); // the same, as ffmpeg reads it, e.g. "30000/1001"
// Show frame idx: 1 when shown, 0 when not ready yet (call again next loop), -1 past the end
int showVideoFrame(int idx);
