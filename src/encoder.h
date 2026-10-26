// Video encoder for the export. Desktop: the ffmpeg program (encoder_desktop.cpp).
// Browser: Mediabunny (MP4, through WebCodecs) or gifenc, loaded from a CDN (encoder_web.cpp).
#pragma once

#include <string>
#include <vector>

enum VideoFormat { V_MP4, V_GIF };

// Get ready for an export (browser: start loading the library); encoderBusy() until then
void encoderPrepare(VideoFormat format);

// Start encoding w x h frames at `rate` fps as ffmpeg reads it ("30000/1001"; `fps` is the
// same number). Desktop writes the file to `path`; the browser downloads it under that name.
bool encoderOpen(VideoFormat format, const std::string& path, int w, int h, const std::string& rate, double fps);

// True while the encoder can't take another frame yet
bool encoderBusy();

// One frame: RGBA, bottom row first (as glReadPixels gives it)
bool encoderWrite(const std::vector<unsigned char>& rgba);

// No more frames: finish the file in the background
void encoderClose();

// 0 while working, 1 once the file is done (after encoderClose), -1 after a failure
int encoderPoll();
std::string encoderError();

// Stop and throw the output away
void encoderAbort();
