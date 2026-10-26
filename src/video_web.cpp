// Video in the browser: a hidden <video> element decodes and plays it (looping).
// Once per loop the frame it is showing is drawn to a canvas and its pixels are
// copied here, so when converting is slower than the video, frames are skipped.
// Opening is asynchronous: webVideoReady or webVideoFailed is called later.
// The browser doesn't say a video's frame rate, so the export samples it at EXPORT_FPS.
#include "media.h"
#include "app.h"

#include <SDL3/SDL.h>
#include <emscripten.h>

const char* OPEN_ERROR = "Could not open the file.\nThis browser can't play this video format.";

struct Video
{
    bool open = false; // a video is loaded and playing in the <video> element
    int w = 0, h = 0;
};
static Video g_video;

static std::vector<unsigned char> g_videoFrame; // the video frame the main thread is converting

static const double EXPORT_FPS = 30;
static const char* EXPORT_RATE = "30"; // the same, as text

EM_JS(void, jsVideoClose, (), {
    const v = Module.asciiVideo;
    if (!v) return;
    Module.asciiVideo = null;
    v.closed = true;
    v.onloadeddata = v.onerror = v.onseeked = null;
    v.pause();
    URL.revokeObjectURL(v.src);
    v.removeAttribute('src');
    v.load();
});

EM_JS(void, jsVideoOpen, (const char* path), {
    const v = document.createElement('video');
    v.muted = true;
    v.loop = true;
    v.playsInline = true;
    v.src = URL.createObjectURL(new Blob([FS.readFile(UTF8ToString(path))]));
    v.fresh = true; // a frame we have not copied yet is on screen
    v.onloadeddata = () => {
        v.onloadeddata = null;
        _webVideoReady(v.videoWidth, v.videoHeight, isFinite(v.duration) ? v.duration : 0);
    };
    v.onerror = () => {
        if (Module.asciiVideo == v) _webVideoFailed();
    };
    v.onseeked = () => { v.fresh = true; v.seekDone = true; };
    if (v.requestVideoFrameCallback) {
        const onFrame = () => {
            if (v.closed) return;
            v.fresh = true;
            v.requestVideoFrameCallback(onFrame);
        };
        v.requestVideoFrameCallback(onFrame);
    }
    Module.asciiVideo = v;
});

EM_JS(double, jsVideoTime, (), {
    const v = Module.asciiVideo;
    return v ? v.currentTime : 0;
});

EM_JS(void, jsVideoSeek, (double t), {
    const v = Module.asciiVideo;
    if (!v) return;
    v.stepTime = undefined;
    v.currentTime = t;
});

// For the export: pauses and seeks to `t`. Returns 1 once the frame at `t` is on screen.
EM_JS(int, jsVideoStep, (double t), {
    const v = Module.asciiVideo;
    if (!v) return 0;
    if (v.stepTime !== t) {
        v.stepTime = t;
        v.seekDone = false;
        v.pause();
        v.currentTime = t;
        return 0;
    }
    return v.seekDone && !v.seeking && v.readyState >= 2 ? 1 : 0;
});

// Plays or pauses the video. Returns 1 when the frame on screen changed since the last call.
EM_JS(int, jsVideoUpdate, (int playing), {
    const v = Module.asciiVideo;
    if (!v || v.readyState < 2) return 0;
    if (playing && v.paused) v.play().catch(() => {});
    if (!playing && !v.paused) v.pause();
    // Without requestVideoFrameCallback, a moved clock means a new frame
    if (!v.requestVideoFrameCallback && v.currentTime != v.lastTime) v.fresh = true;
    v.lastTime = v.currentTime;
    if (!v.fresh) return 0;
    v.fresh = false;
    return 1;
});

// Copies the frame on screen to `dst` (w x h RGBA)
EM_JS(void, jsVideoCopy, (unsigned char* dst, int w, int h), {
    const v = Module.asciiVideo;
    let c = Module.asciiCanvas;
    if (!c) {
        c = Module.asciiCanvas = document.createElement('canvas');
        c.ctx = c.getContext('2d', { willReadFrequently: true });
    }
    if (c.width != w || c.height != h) {
        c.width = w;
        c.height = h;
    }
    c.ctx.drawImage(v, 0, 0, w, h);
    HEAPU8.set(c.ctx.getImageData(0, 0, w, h).data, dst);
});

void stopVideo()
{
    jsVideoClose();
    g_video.open = false;
}

// Seek to `t` seconds
bool startVideo(double t)
{
    jsVideoSeek(t);
    return true;
}

bool loadVideo(const std::string& path)
{
    stopVideo();
    jsVideoOpen(path.c_str());
    return true;
}

extern "C" EMSCRIPTEN_KEEPALIVE void webVideoReady(int w, int h, double duration)
{
    Video& v = g_video;
    v.open = true;
    v.w = w;
    v.h = h;
    startMedia(M_VIDEO, duration);
    createPreview(w, h);
    g_videoFrame.assign((size_t)w * h * 4, 0);
}

extern "C" EMSCRIPTEN_KEEPALIVE void webVideoFailed()
{
    stopVideo();
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Error", OPEN_ERROR, g_window);
}

// The browser keeps the clock; convert the frame it is showing now
void updateVideo(double)
{
    Video& v = g_video;
    if (!v.open) return;
    if (g_playing) g_time = jsVideoTime();
    if (!jsVideoUpdate(g_playing)) return;
    jsVideoCopy(g_videoFrame.data(), v.w, v.h);
    showFrame(g_videoFrame.data());
}

double videoFps() { return EXPORT_FPS; }
const char* videoRate() { return EXPORT_RATE; }

int showVideoFrame(int idx)
{
    Video& v = g_video;
    double t = (idx + 0.5) / EXPORT_FPS; // mid-frame, clear of rounding at frame changes
    if (!v.open || t >= g_duration) return -1;
    if (!jsVideoStep(t)) return 0;
    jsVideoCopy(g_videoFrame.data(), v.w, v.h);
    g_frameShown = idx;
    showFrame(g_videoFrame.data());
    return 1;
}
