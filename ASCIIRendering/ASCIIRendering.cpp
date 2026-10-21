// Build (Windows, macOS, Linux, web): see README.md
//   cmake -S . -B build && cmake --build build
//
// Open an image, GIF or video, then drag the sliders to see the ASCII output
// change live. Videos are decoded by the ffmpeg program, which must be on PATH;
// in the browser build (__EMSCRIPTEN__) the browser decodes them instead.

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_WINDOWS_UTF8 // SDL hands us UTF-8 paths, also on Windows
#include "stb_image.h"
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include "font_data.h" // DejaVu Sans Mono, embedded at build time (assets/)

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#else
#include <atomic>
#include <condition_variable>
#include <thread>
#endif

// ---------------------------------------------------------------------------
// Basic types
// ---------------------------------------------------------------------------
const int N = 6; // sampling circles per cell
using Vec = std::array<float, N>;

struct Gray
{
    int w = 0, h = 0;
    std::vector<float> px; // lightness 0..1
};

struct Circle { float cx, cy, r; }; // cx, cy: fraction of cell; r: fraction of cell width

const int ECC = 10; // external circles per cell
using ExtVec = std::array<float, ECC>;
Circle g_extCircles[ECC];

// which external circles affect an internal circle
const std::vector<int> AFFECTING[N] = {
    {0, 1, 2, 4},
    {0, 1, 3, 5},
    {2, 4, 6},
    {3, 5, 7},
    {4, 6, 8, 9},
    {5, 7, 8, 9},
};

struct Glyph
{
    char ch;
    Vec v;
};

// ---------------------------------------------------------------------------
// Parameters shown as sliders
// ---------------------------------------------------------------------------
enum { P_COLS, P_STEP, P_CONTRAST, P_RADIUS, P_STAGGER, P_FONT, P_COUNT };

struct Param
{
    const char* name;
    int minv, maxv, init; // slider range (integers)
    float scale;          // real value = slider position * scale
    int decimals;         // decimals to show in the label
    int pos;              // current slider position
};

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

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
const float PANEL_W = 300; // before DPI scaling

SDL_Window* g_window = nullptr;
SDL_Renderer* g_renderer = nullptr;
SDL_Texture* g_image = nullptr;      // for the preview
Gray g_gray;                         // grayscale copy for sampling
Circle g_circles[N];
std::vector<Glyph> g_glyphs;
std::vector<std::string> g_lines;    // current ASCII output
stbtt_fontinfo g_font;               // glyph font for the shape vectors

// ---------------------------------------------------------------------------
// File picker. SDL may call back on another thread, so the main loop picks
// the result up from here.
// ---------------------------------------------------------------------------
std::mutex g_pickedMutex;
std::string g_picked;

#ifdef __EMSCRIPTEN__
// Browser: an <input type="file"> copies the file into Emscripten's in-memory
// file system, then webFilePicked hands its path to the main loop.
EM_JS(void, PickImageFile, (), {
    const input = document.createElement('input');
    input.type = 'file';
    input.accept = 'image/*,video/*';
    input.onchange = async () => {
        const file = input.files[0];
        if (!file) return;
        const data = new Uint8Array(await file.arrayBuffer());
        try { FS.mkdir('/picked'); } catch (e) {}
        FS.writeFile('/picked/file', data, { canOwn: true });
        _webFilePicked();
    };
    input.click();
});

extern "C" EMSCRIPTEN_KEEPALIVE void webFilePicked()
{
    std::lock_guard<std::mutex> lock(g_pickedMutex);
    g_picked = "/picked/file";
}
#else
void SDLCALL onFilePicked(void*, const char* const* files, int)
{
    if (!files || !files[0]) return; // error or cancelled
    std::lock_guard<std::mutex> lock(g_pickedMutex);
    g_picked = files[0];
}

void PickImageFile()
{
    static const SDL_DialogFileFilter filters[] = {
        { "Images, GIFs and videos", "bmp;jpg;jpeg;png;gif;tga;psd;mp4;mkv;webm;mov;avi;m4v" },
        { "All Files", "*" },
    };
    SDL_ShowOpenFileDialog(onFilePicked, nullptr, g_window, filters, 2, nullptr, false);
}
#endif

// ---------------------------------------------------------------------------
// RGBA pixels -> grayscale buffer
// ---------------------------------------------------------------------------
Gray toGray(const unsigned char* rgba, int w, int h)
{
    Gray g;
    g.w = w;
    g.h = h;
    g.px.resize((size_t)w * h);

    for (int y = 0; y < h; ++y)
    {
        const unsigned char* row = rgba + (size_t)y * w * 4;
        for (int x = 0; x < w; ++x)
        {
            // memory order is R, G, B, A
            float r = row[x * 4 + 0];
            float gg = row[x * 4 + 1];
            float b = row[x * 4 + 2];
            float a = row[x * 4 + 3];
            if(a == 0)
                g.px[(size_t)y * w + x] = 0;
            else
                g.px[(size_t)y * w + x] = (0.2126f * r + 0.7152f * gg + 0.0722f * b) / 255.0f * (a / 255.0f);
        }
    }
    return g;
}

// ---------------------------------------------------------------------------
// The 6 circles. Index order:  0 1
//                              2 3
//                              4 5
// Left circles are lowered by `stagger`, right circles raised by it.
// ---------------------------------------------------------------------------
void layoutCircles(float radius, float stagger)
{
    // Center x of the left and right columns. Symmetric around the middle
    // (0.27 + 0.73 = 1). Magic numbers: picked by eye so the two columns
    // sit apart but still cover the cell. Not from the blog.
    const float xs[2] = { 0.27f, 0.73f };

    // Center y of the top, middle and bottom rows, before staggering.
    // Evenly spread and symmetric (0.18 + 0.82 = 1), middle at 0.50.
    // Also picked by eye. Change them and check the circles still cover the cell.
    const float ys[3] = { 0.18f, 0.50f, 0.82f };

    for (int row = 0; row < 3; ++row)
    {
        for (int col = 0; col < 2; ++col)
        {
            Circle& c = g_circles[row * 2 + col];
            c.cx = xs[col];
            c.cy = ys[row] + (col == 0 ? stagger : -stagger);
            c.r = radius;
        }
    }

    const float ex[ECC] = {  0.27f,  0.73f,  0.27f,  0.73f, -0.20f, -0.20f, -0.20f, 1.20f, 1.20f, 1.20f };
    const float ey[ECC] = { -0.15f, -0.15f,  1.15f,  1.15f,  0.20f,  0.50f,  0.80f, 0.20f, 0.50f, 0.80f };
    for (int i = 0; i < ECC; ++i)
        g_extCircles[i] = { ex[i], ey[i], radius };
}


// Average lightness of the pixels inside one circle.
// `step` = supersampling knob (1 = every pixel, 2 = every other pixel, ...)
float sampleCircle(const Gray& g, float ox, float oy, float cw, float ch,
    const Circle& c, int step)
    {
        float cx = ox + c.cx * cw;
        float cy = oy + c.cy * ch;
        float r = c.r * cw;
        
        int x0 = (int)std::floor(cx - r), x1 = (int)std::ceil(cx + r);
        int y0 = (int)std::floor(cy - r), y1 = (int)std::ceil(cy + r);
        
        float sum = 0.0f;
        int count = 0;
        
        for (int y = y0; y <= y1; y += step)
        {
            for (int x = x0; x <= x1; x += step)
            {
                if (x < 0 || y < 0 || x >= g.w || y >= g.h) continue;
                
                float dx = x + 0.5f - cx;
                float dy = y + 0.5f - cy;
                if (dx * dx + dy * dy > r * r) continue; // outside the circle
                
                sum += g.px[(size_t)y * g.w + x];
                ++count;
            }
        }
        
        if (count > 0) return sum / count;
        
        // Circle was tiny or the step skipped everything: fall back to the center pixel.
        int px = (int)cx, py = (int)cy;
        if (px >= 0 && py >= 0 && px < g.w && py < g.h) return g.px[(size_t)py * g.w + px];
        return 0.0f;
    }

    ExtVec sampleExternal(const Gray& g, float ox, float oy, float cw, float ch, int step)
    {
        ExtVec e;
        for(int i = 0; i < ECC; i++)
            e[i] = sampleCircle(g, ox, oy, cw, ch, g_extCircles[i], step);
        return e;
    }
    
    Vec sampleCell(const Gray& g, float ox, float oy, float cw, float ch, int step)
    {
        Vec v;
        for (int i = 0; i < N; ++i)
        v[i] = sampleCircle(g, ox, oy, cw, ch, g_circles[i], step);
    return v;
}

// ---------------------------------------------------------------------------
// Character shape vectors
// ---------------------------------------------------------------------------
std::vector<Glyph> buildGlyphs(float fontScale)
{
    const int gw = 8 * 4;   // glyph cell 8x16, rendered 4x bigger
    const int gh = 16 * 4;

    // Font size in pixels per em, like GDI+ UnitPixel
    float scale = stbtt_ScaleForMappingEmToPixels(&g_font, gh * fontScale);
    int ascent, descent, lineGap;
    stbtt_GetFontVMetrics(&g_font, &ascent, &descent, &lineGap);
    // Center the line box vertically (ascent to descent), like StringAlignmentCenter
    float baseline = (gh - (ascent - descent) * scale) / 2 + ascent * scale;

    Gray gray;
    gray.w = gw;
    gray.h = gh;

    std::vector<Glyph> glyphs;

    for (char c = 32; c < 127; ++c)
    {
        gray.px.assign((size_t)gw * gh, 0.0f); // black cell

        // Center the character's advance horizontally
        int advance, lsb;
        stbtt_GetCodepointHMetrics(&g_font, c, &advance, &lsb);
        float x = (gw - advance * scale) / 2;

        // Draw white text: coverage 0..255 becomes lightness 0..1
        int ox = (int)std::floor(x), oy = (int)std::floor(baseline);
        int bw, bh, xoff, yoff;
        unsigned char* bmp = stbtt_GetCodepointBitmapSubpixel(&g_font, scale, scale,
            x - ox, baseline - oy, c, &bw, &bh, &xoff, &yoff);
        for (int y = 0; y < bh; ++y)
        {
            for (int xx = 0; xx < bw; ++xx)
            {
                int px = ox + xoff + xx, py = oy + yoff + y;
                if (px < 0 || py < 0 || px >= gw || py >= gh) continue;
                gray.px[(size_t)py * gw + px] = bmp[y * bw + xx] / 255.0f;
            }
        }
        stbtt_FreeBitmap(bmp, nullptr);

        Glyph gl;
        gl.ch = c;
        gl.v = sampleCell(gray, 0, 0, (float)gw, (float)gh, 1);
        glyphs.push_back(gl);
    }

    // Normalize each component by its max across all glyphs
	Vec maxv; // Maximum that circle reached across all glyphs, for each circle
    maxv.fill(0.0f);
    for (const Glyph& gl : glyphs)
        for (int i = 0; i < N; ++i)
            if (gl.v[i] > maxv[i]) maxv[i] = gl.v[i];

    for (Glyph& gl : glyphs)
        for (int i = 0; i < N; ++i)
            if (maxv[i] > 0.0f) gl.v[i] /= maxv[i];

    return glyphs;
}

// Global contrast enhancement
void enhanceContrast(Vec& v, float exponent)
{
    float m = *std::max_element(v.begin(), v.end());
    if (m <= 0.0f) return;
    for (int i = 0; i < N; ++i)
        v[i] = std::pow(v[i] / m, exponent) * m;
}

void enhanceDirectional(Vec& v, const ExtVec& ext, float exponent)
{
    for (int i = 0; i < N; ++i)
    {
        float m = v[i];                          // the circle's own value
        for (int e : AFFECTING[i])               // plus the external circles that affect it
            if (ext[e] > m) m = ext[e];
        if (m > 0.0f) v[i] = std::pow(v[i] / m, exponent) * m;
    }
}

// ---------------------------------------------------------------------------
// Nearest neighbor lookup.
//
// `in` is the 6 numbers measured for one image cell (the sampling vector).
// Each glyph has its own 6 numbers (its shape vector). We want the glyph
// whose 6 numbers are closest to `in`, i.e. whose shape looks most like
// the cell.
//
// "Closest" = smallest distance between the two 6-number lists, like the
// distance between two points, just with 6 coordinates instead of 2:
//     distance = sqrt( (a0-b0)^2 + (a1-b1)^2 + ... + (a5-b5)^2 )
// We skip the sqrt: if one squared distance is smaller than another, so is
// its square root, so the winner is the same and we save the work.
//
// Brute force: checks all 95 glyphs for every cell. Fine for a still image.
// ---------------------------------------------------------------------------
char findBestChar(const std::vector<Glyph>& glyphs, const Vec& in)
{
    char best = ' ';        // winner so far (any default, it gets replaced)
    float bestDist = 1e30f; // smallest distance so far; starts huge so the first glyph always beats it

    for (const Glyph& gl : glyphs)
    {
        float d = 0.0f;
        for (int i = 0; i < N; ++i)
        {
            float diff = gl.v[i] - in[i];
            d += diff * diff;
        }
        if (d < bestDist)
        {
            bestDist = d;
            best = gl.ch;
        }
    }
    return best;
}

// ---------------------------------------------------------------------------
// Image -> lines of ASCII, using the current slider values
// ---------------------------------------------------------------------------
void renderAscii()
{
    g_lines.clear();
    if (g_gray.w == 0) return;

    int cols = (int)(val(P_COLS) + 0.5f);
    int step = (int)(val(P_STEP) + 0.5f);
    float exponent = val(P_CONTRAST);

    float cw = (float)g_gray.w / cols;
    float ch = cw * 2.0f; // console characters are ~1:2
    int rows = (int)(g_gray.h / ch);

    for (int r = 0; r < rows; ++r)
    {
        std::string line;
        for (int c = 0; c < cols; ++c)
        {
            Vec v = sampleCell(g_gray, c * cw, r * ch, cw, ch, step);
            if (exponent > 1.001f) 
            {
                ExtVec ext = sampleExternal(g_gray, c * cw, r * ch, cw, ch, step);
                enhanceDirectional(v, ext, exponent);
                enhanceContrast(v, exponent);
            }
            line += findBestChar(g_glyphs, v);
        }
        g_lines.push_back(line);
    }
}

// ---------------------------------------------------------------------------
// UI helpers
// ---------------------------------------------------------------------------
void onParamsChanged()
{
    // Glyph shape vectors only depend on these three, so only rebuild if they changed
    float r = val(P_RADIUS), s = val(P_STAGGER), f = val(P_FONT);
    static float lr = -1, ls = -1, lf = -1;
    if (r != lr || s != ls || f != lf)
    {
        layoutCircles(r, s);
        g_glyphs = buildGlyphs(f);
        lr = r; ls = s; lf = f;
    }

    renderAscii();
}

// ---------------------------------------------------------------------------
// Media: a still image, a GIF or a video. Animated media is converted one
// frame at a time with the same code as a still image (showFrame).
// ---------------------------------------------------------------------------
enum MediaKind { M_NONE, M_IMAGE, M_GIF, M_VIDEO };

MediaKind g_kind = M_NONE;
int g_mediaW = 0, g_mediaH = 0;
bool g_playing = false;
double g_time = 0;      // playback position, seconds
double g_duration = 0;  // 0 = unknown
int g_frameShown = -1;  // index of the frame in g_gray / g_lines

// GIF: every frame is decoded up front
std::vector<unsigned char> g_gifPixels; // RGBA, frame after frame
std::vector<double> g_gifStart;         // start time of each frame, seconds

std::vector<unsigned char> g_videoFrame; // the video frame the main thread is converting

void createPreview(int w, int h, SDL_TextureAccess access)
{
    SDL_DestroyTexture(g_image);
    g_image = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ABGR8888, access, w, h);
    g_mediaW = w;
    g_mediaH = h;
}

void showFrame(const unsigned char* rgba)
{
    if (g_image) SDL_UpdateTexture(g_image, nullptr, rgba, g_mediaW * 4);
    g_gray = toGray(rgba, g_mediaW, g_mediaH);
    renderAscii();
}

void startMedia(MediaKind kind, double duration)
{
    g_kind = kind;
    g_duration = duration;
    g_time = 0;
    g_playing = kind == M_GIF || kind == M_VIDEO;
    g_frameShown = -1;
    g_gray = Gray();
    g_lines.clear();
}

#ifdef __EMSCRIPTEN__
// Video in the browser: a hidden <video> element decodes and plays it (looping).
// Once per loop the frame it is showing is drawn to a canvas and its pixels are
// copied here, so when converting is slower than the video, frames are skipped.
// Opening is asynchronous: webVideoReady or webVideoFailed is called later.
const char* OPEN_ERROR = "Could not open the file.\nThis browser can't play this video format.";

struct Video
{
    bool open = false; // a video is loaded and playing in the <video> element
    int w = 0, h = 0;
};
Video g_video;

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
    v.onseeked = () => { v.fresh = true; };
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
    if (v) v.currentTime = t;
});

// Plays or pauses the video, then copies the frame on screen to `dst` (w x h RGBA)
// if it changed since the last call. Returns 1 when it did.
EM_JS(int, jsVideoUpdate, (int playing, unsigned char* dst, int w, int h), {
    const v = Module.asciiVideo;
    if (!v || v.readyState < 2) return 0;
    if (playing && v.paused) v.play().catch(() => {});
    if (!playing && !v.paused) v.pause();
    // Without requestVideoFrameCallback, a moved clock means a new frame
    if (!v.requestVideoFrameCallback && v.currentTime != v.lastTime) v.fresh = true;
    v.lastTime = v.currentTime;
    if (!v.fresh) return 0;
    v.fresh = false;

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
    return 1;
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
    createPreview(w, h, SDL_TEXTUREACCESS_STREAMING);
    g_videoFrame.assign((size_t)w * h * 4, 0);
}

extern "C" EMSCRIPTEN_KEEPALIVE void webVideoFailed()
{
    stopVideo();
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Error", OPEN_ERROR, g_window);
}
#else
const char* OPEN_ERROR = "Could not open the file.\nVideos need ffmpeg and ffprobe installed (on PATH).";

// Video: the ffmpeg program decodes it and writes raw RGBA frames to a pipe.
// A reader thread holds back each frame until the playback clock reaches it,
// so when converting is slower than the video, frames are skipped.
struct Video
{
    std::string path;
    int w = 0, h = 0;
    double fps = 0;
    std::string rate;            // fps as ffprobe wrote it, e.g. "30000/1001"
    SDL_Process* proc = nullptr;
    std::thread reader;
    std::mutex m;
    std::condition_variable cv;
    std::vector<unsigned char> frame; // latest frame read, guarded by m
    int frameIdx = -1;                // its index, -1 = none yet
    int wanted = 0;                   // reader may hand over frames up to this index
    int first = 0;                    // index of the first frame ffmpeg sends (seek point)
    bool eof = false;
    std::atomic<bool> stop{ false };
};
Video g_video;

void videoReader()
{
    Video& v = g_video;
    SDL_IOStream* out = SDL_GetProcessOutput(v.proc);
    size_t size = (size_t)v.w * v.h * 4;
    std::vector<unsigned char> buf(size);

    for (int idx = v.first; ; ++idx)
    {
        size_t got = 0;
        while (got < size && !v.stop)
        {
            size_t n = SDL_ReadIO(out, buf.data() + got, size - got);
            got += n;
            if (n > 0) continue;
            if (SDL_GetIOStatus(out) != SDL_IO_STATUS_NOT_READY) break; // end of video or error
            SDL_Delay(1);
        }

        std::unique_lock<std::mutex> lock(v.m);
        if (got < size)
        {
            v.eof = true;
            return;
        }
        v.cv.wait(lock, [&] { return v.stop || idx <= v.wanted; });
        if (v.stop) return;
        v.frame.swap(buf);
        v.frameIdx = idx;
    }
}

void stopVideo()
{
    Video& v = g_video;
    if (!v.proc) return;
    {
        std::lock_guard<std::mutex> lock(v.m);
        v.stop = true;
    }
    v.cv.notify_all();
    SDL_KillProcess(v.proc, true);
    if (v.reader.joinable()) v.reader.join();
    SDL_WaitProcess(v.proc, true, nullptr);
    SDL_DestroyProcess(v.proc);
    v.proc = nullptr;
}

// (Re)start ffmpeg at `t` seconds
bool startVideo(double t)
{
    stopVideo();
    Video& v = g_video;
    std::string ss = std::to_string(t);
    std::string vf = "fps=" + v.rate; // constant frame rate, so frame index = time * fps
    const char* args[] = { "ffmpeg", "-v", "error", "-nostdin", "-ss", ss.c_str(),
        "-i", v.path.c_str(), "-map", "0:v:0", "-vf", vf.c_str(),
        "-f", "rawvideo", "-pix_fmt", "rgba", "-", nullptr };
    v.proc = SDL_CreateProcess(args, true);
    if (!v.proc) return false;

    v.first = (int)std::lround(t * v.fps);
    v.wanted = v.first;
    v.frameIdx = -1;
    v.eof = false;
    v.stop = false;
    v.frame.assign((size_t)v.w * v.h * 4, 0);
    g_frameShown = -1;
    v.reader = std::thread(videoReader);
    return true;
}

double parseRate(const char* s)
{
    int num = 0, den = 0;
    if (std::sscanf(s, "%d/%d", &num, &den) != 2 || num <= 0 || den <= 0) return 0;
    return (double)num / den;
}

// Size, frame rate and length of the first video stream, from ffprobe
bool probeVideo(const std::string& path, Video& v, double& duration)
{
    const char* args[] = { "ffprobe", "-v", "error", "-select_streams", "v:0",
        "-show_entries", "stream=width,height,avg_frame_rate,r_frame_rate:stream_side_data=rotation:format=duration",
        "-of", "default=noprint_wrappers=1", path.c_str(), nullptr };
    SDL_Process* proc = SDL_CreateProcess(args, true);
    if (!proc) return false;
    size_t len = 0;
    int exitCode = -1;
    char* out = (char*)SDL_ReadProcess(proc, &len, &exitCode);
    SDL_DestroyProcess(proc);
    if (!out) return false;

    int w = 0, h = 0, rotation = 0;
    double avgRate = 0, baseRate = 0;
    std::string avgText, baseText;
    duration = 0;

    std::string text(out, len);
    SDL_free(out);
    size_t pos = 0;
    while (pos < text.size())
    {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        if (!value.empty() && value.back() == '\r') value.pop_back();

        if (key == "width") w = std::atoi(value.c_str());
        else if (key == "height") h = std::atoi(value.c_str());
        else if (key == "rotation") rotation = std::atoi(value.c_str());
        else if (key == "duration") duration = std::atof(value.c_str()); // "N/A" -> 0
        else if (key == "avg_frame_rate") { avgRate = parseRate(value.c_str()); avgText = value; }
        else if (key == "r_frame_rate") { baseRate = parseRate(value.c_str()); baseText = value; }
    }

    if (exitCode != 0 || w <= 0 || h <= 0) return false;
    if (avgRate <= 0 && baseRate <= 0) return false; // still image ffmpeg can read, not a video

    // ffmpeg turns rotated (phone) videos upright, so the frames come out with the sides swapped
    if (std::abs(rotation) % 180 == 90) std::swap(w, h);

    v.path = path;
    v.w = w;
    v.h = h;
    v.fps = avgRate > 0 ? avgRate : baseRate;
    v.rate = avgRate > 0 ? avgText : baseText;
    return true;
}

bool loadVideo(const std::string& path)
{
    Video probed;
    double duration;
    if (!probeVideo(path, probed, duration)) return false;

    stopVideo();
    Video& v = g_video;
    v.path = probed.path;
    v.w = probed.w;
    v.h = probed.h;
    v.fps = probed.fps;
    v.rate = probed.rate;
    startMedia(M_VIDEO, duration);
    createPreview(v.w, v.h, SDL_TEXTUREACCESS_STREAMING);
    g_videoFrame.assign((size_t)v.w * v.h * 4, 0);
    return startVideo(0);
}
#endif

bool loadImage(const std::string& path)
{
    int w, h, n;
    unsigned char* rgba = stbi_load(path.c_str(), &w, &h, &n, 4);
    if (!rgba) return false;

    stopVideo();
    startMedia(M_IMAGE, 0);
    createPreview(w, h, SDL_TEXTUREACCESS_STATIC);
    showFrame(rgba);
    stbi_image_free(rgba);
    return true;
}

bool isGif(const std::string& path)
{
    char magic[4] = {};
    SDL_IOStream* io = SDL_IOFromFile(path.c_str(), "rb");
    if (!io) return false;
    SDL_ReadIO(io, magic, 4);
    SDL_CloseIO(io);
    return std::memcmp(magic, "GIF8", 4) == 0;
}

bool loadGif(const std::string& path)
{
    size_t len = 0;
    void* file = SDL_LoadFile(path.c_str(), &len);
    if (!file) return false;
    int* delays = nullptr;
    int w, h, frames, n;
    unsigned char* rgba = stbi_load_gif_from_memory((const stbi_uc*)file, (int)len,
        &delays, &w, &h, &frames, &n, 4);
    SDL_free(file);
    if (!rgba) return false;

    stopVideo();
    g_gifPixels.assign(rgba, rgba + (size_t)w * h * 4 * frames);
    g_gifStart.clear();
    double t = 0;
    for (int i = 0; i < frames; ++i)
    {
        g_gifStart.push_back(t);
        int ms = delays ? delays[i] : 0;
        t += (ms <= 10 ? 100 : ms) / 1000.0; // browsers also play 0-10 ms delays at 100 ms
    }
    stbi_image_free(rgba);
    stbi_image_free(delays);

    startMedia(frames > 1 ? M_GIF : M_IMAGE, t);
    createPreview(w, h, SDL_TEXTUREACCESS_STREAMING);
    showFrame(g_gifPixels.data());
    g_frameShown = 0;
    return true;
}

void openFile(const std::string& path)
{
    bool ok;
    if (isGif(path)) ok = loadGif(path);
    else if (int w, h, n; stbi_info(path.c_str(), &w, &h, &n)) ok = loadImage(path);
    else ok = loadVideo(path);

#ifdef __EMSCRIPTEN__
    // The file was copied into memory for us and has been read now, so free it
    std::remove(path.c_str());
#endif

    if (!ok)
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Error", OPEN_ERROR, g_window);
}

// Move the playback clock and convert the frame it lands on, once per loop
void updatePlayback()
{
    static Uint64 last = SDL_GetTicksNS();
    Uint64 now = SDL_GetTicksNS();
    double dt = (double)(now - last) / 1e9;
    last = now;

    if (g_kind == M_GIF)
    {
        if (g_playing) g_time = std::fmod(g_time + dt, g_duration);
        int f = (int)(std::upper_bound(g_gifStart.begin(), g_gifStart.end(), g_time) - g_gifStart.begin()) - 1;
        f = std::clamp(f, 0, (int)g_gifStart.size() - 1);
        if (f != g_frameShown)
        {
            showFrame(&g_gifPixels[(size_t)f * g_mediaW * g_mediaH * 4]);
            g_frameShown = f;
        }
    }
#ifdef __EMSCRIPTEN__
    else if (g_kind == M_VIDEO && g_video.open)
    {
        // The browser keeps the clock; convert the frame it is showing now
        Video& v = g_video;
        if (g_playing) g_time = jsVideoTime();
        if (jsVideoUpdate(g_playing, g_videoFrame.data(), v.w, v.h)) showFrame(g_videoFrame.data());
    }
#else
    else if (g_kind == M_VIDEO && g_video.proc)
    {
        Video& v = g_video;
        if (g_playing) g_time += dt;

        bool fresh = false, ended = false, empty = false;
        {
            std::lock_guard<std::mutex> lock(v.m);
            v.wanted = std::max(v.first, (int)(g_time * v.fps));
            if (v.frameIdx >= 0 && v.frameIdx != g_frameShown)
            {
                v.frame.swap(g_videoFrame);
                g_frameShown = v.frameIdx;
                fresh = true;
            }
            // Ended once the clock is past the last frame ffmpeg sent
            ended = v.eof && v.frameIdx == g_frameShown && g_time * v.fps >= v.frameIdx + 1;
            empty = v.eof && v.frameIdx < 0;
        }
        v.cv.notify_all();

        if (fresh) showFrame(g_videoFrame.data());
        if (ended || empty)
        {
            if (empty && v.first == 0) g_playing = false; // ffmpeg could not decode anything
            else if (g_playing)
            {
                g_time = 0;
                startVideo(0); // loop
            }
            else
            {
                g_time = std::min(g_time, (double)(v.frameIdx + 1) / v.fps);
            }
        }
    }
#endif
}

#ifdef __EMSCRIPTEN__
// SDL keeps its clipboard to itself in the browser, so use the page's
EM_JS(void, setClipboardText, (const char* text), {
    navigator.clipboard.writeText(UTF8ToString(text)).catch(() => {});
});
#else
void setClipboardText(const char* text)
{
    SDL_SetClipboardText(text);
}
#endif

void copyToClipboard()
{
    if (g_lines.empty()) return;

    std::string text;
    for (const std::string& l : g_lines) { text += l; text += "\n"; }
    setClipboardText(text.c_str());
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------
void paintAscii(ImFont* font)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImVec2 size = ImGui::GetContentRegionAvail();
    int aw = (int)size.x;
    int ah = (int)size.y;
    if (aw <= 0 || ah <= 0) return;

    dl->AddRectFilled(p, ImVec2(p.x + aw, p.y + ah), IM_COL32(0, 0, 0, 255));

    if (g_lines.empty())
    {
        dl->AddText(ImVec2(p.x + 20, p.y + 20), IM_COL32(160, 160, 160, 255),
            "Click \"Open file...\" or drop an image, GIF or video here to start");
        return;
    }

    int rows = (int)g_lines.size();
    int cols = (int)g_lines[0].size();

    // Largest character cell (width cell, height 2*cell) that fits the area
    int c1 = aw / cols;
    int c2 = ah / (rows * 2);
    int cell = c1 < c2 ? c1 : c2;
    if (cell < 2) cell = 2;

    // Font size whose character advance is exactly one cell
    float advance = font->CalcTextSizeA(100.0f, FLT_MAX, 0.0f, "M").x / 100.0f;
    float fontSize = cell / advance;

    for (int i = 0; i < rows; ++i)
        dl->AddText(font, fontSize, ImVec2(p.x, p.y + (float)(i * cell * 2)),
            IM_COL32(255, 255, 255, 255), g_lines[i].c_str());
}

void paintPreview()
{
    if (!g_image) return;

    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.y < 20) return;

    float iw, ih;
    SDL_GetTextureSize(g_image, &iw, &ih);
    float scale = avail.x / iw;
    if (avail.y / ih < scale) scale = avail.y / ih;

    ImGui::Image((ImTextureID)(intptr_t)g_image, ImVec2(iw * scale, ih * scale));
}

void drawUi(float dpi, ImFont* font)
{
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::Begin("ASCII renderer", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleVar(2);

    // Left panel: buttons, sliders, preview
    ImGui::BeginChild("panel", ImVec2(PANEL_W * dpi, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2;
    if (ImGui::Button("Open file...", ImVec2(half, 0)))
        PickImageFile();
    ImGui::SameLine();
    if (ImGui::Button("Copy text", ImVec2(half, 0)))
        copyToClipboard();
    ImGui::Spacing();

    if (g_kind == M_GIF || g_kind == M_VIDEO)
    {
        if (ImGui::Button(g_playing ? "Pause" : "Play", ImVec2(half, 0)))
            g_playing = !g_playing;
        ImGui::SameLine();
        int t = (int)g_time, d = (int)g_duration;
        ImGui::Text("%d:%02d / %d:%02d", t / 60, t % 60, d / 60, d % 60);

        // Seek. A GIF follows the slider live; a video seeks (restarts ffmpeg) when it is let go.
        static bool wasPlaying = false;
        float pos = (float)std::min(g_time, g_duration);
        ImGui::BeginDisabled(g_duration <= 0);
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderFloat("##seek", &pos, 0.0f, (float)g_duration, "");
        ImGui::EndDisabled();
        if (ImGui::IsItemActivated()) { wasPlaying = g_playing; g_playing = false; }
        if (ImGui::IsItemActive()) g_time = pos;
        if (ImGui::IsItemDeactivated())
        {
            if (g_kind == M_VIDEO) startVideo(g_time);
            g_playing = wasPlaying;
        }
        ImGui::Spacing();
    }

    bool changed = false;
    for (int i = 0; i < P_COUNT; ++i)
    {
        ImGui::Text("%s: %.*f", P[i].name, P[i].decimals, val(i));
        ImGui::PushID(i);
        ImGui::SetNextItemWidth(-1);
        changed |= ImGui::SliderInt("##slider", &P[i].pos, P[i].minv, P[i].maxv, "");
        ImGui::PopID();
    }
    if (changed) onParamsChanged();

    ImGui::Spacing();
    paintPreview();
    ImGui::EndChild();

    // Right: the ASCII output
    ImGui::SameLine(0, 0);
    ImGui::BeginChild("ascii", ImVec2(0, 0), ImGuiChildFlags_None,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    paintAscii(font);
    ImGui::EndChild();

    ImGui::End();
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------
float g_dpi = 1;
ImFont* g_uiFont = nullptr;
bool g_done = false;

void handleEvent(const SDL_Event& e)
{
    ImGui_ImplSDL3_ProcessEvent(&e);
    if (e.type == SDL_EVENT_QUIT) g_done = true;
    if (e.type == SDL_EVENT_DROP_FILE) openFile(e.drop.data);
}

// Everything after the events: open a picked file, advance playback, draw
void runFrame()
{
    std::string picked;
    {
        std::lock_guard<std::mutex> lock(g_pickedMutex);
        picked.swap(g_picked);
    }
    if (!picked.empty()) openFile(picked);

    if (SDL_GetWindowFlags(g_window) & SDL_WINDOW_MINIMIZED) return;

    updatePlayback();

    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    drawUi(g_dpi, g_uiFont);
    ImGui::Render();

    ImGuiIO& io = ImGui::GetIO();
    SDL_SetRenderScale(g_renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
    SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
    SDL_RenderClear(g_renderer);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), g_renderer);
    SDL_RenderPresent(g_renderer);
}

#ifdef __EMSCRIPTEN__
// The browser calls this once per display frame. It must return, or the page freezes.
void webFrame()
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) handleEvent(e);
    runFrame();
}
#endif

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
int main(int argc, char** argv)
{
    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    float dpi = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (dpi <= 0) dpi = 1;
    g_dpi = dpi;
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#ifdef __EMSCRIPTEN__
    flags |= SDL_WINDOW_FILL_DOCUMENT; // the canvas follows the browser window's size
#endif
    g_window = SDL_CreateWindow("ASCII renderer", (int)(1200 * dpi), (int)(800 * dpi), flags);
    g_renderer = g_window ? SDL_CreateRenderer(g_window, nullptr) : nullptr;
    if (!g_renderer)
    {
        SDL_Log("Could not create the window: %s", SDL_GetError());
        return 1;
    }
#ifndef __EMSCRIPTEN__
    SDL_SetRenderVSync(g_renderer, 1); // the browser already paces frames to the display
#endif

    stbtt_InitFont(&g_font, font_data, stbtt_GetFontOffsetForIndex(font_data, 0));

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(dpi);
    style.FontScaleDpi = dpi;

    // Same font for the UI and the ASCII output
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;
    g_uiFont = io.Fonts->AddFontFromMemoryTTF((void*)font_data, (int)sizeof(font_data), 15.0f, &cfg);

    ImGui_ImplSDL3_InitForSDLRenderer(g_window, g_renderer);
    ImGui_ImplSDLRenderer3_Init(g_renderer);

    for (Param& p : P) p.pos = p.init;
    onParamsChanged(); // builds glyphs
    if (argc > 1) openFile(argv[1]);

#ifdef __EMSCRIPTEN__
    // Hand the loop to the browser; the program keeps running after main returns
    emscripten_set_main_loop(webFrame, 0, false);
    return 0;
#else
    while (!g_done)
    {
        // Sleep until something happens; the timeout picks up file dialog results.
        // While playing, don't sleep: vsync paces the loop.
        bool minimized = SDL_GetWindowFlags(g_window) & SDL_WINDOW_MINIMIZED;
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, g_playing && !minimized ? 0 : 100))
        {
            do handleEvent(e);
            while (SDL_PollEvent(&e));
        }
        runFrame();
    }

    stopVideo();
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyTexture(g_image);
    SDL_DestroyRenderer(g_renderer);
    SDL_DestroyWindow(g_window);
    SDL_Quit();
    return 0;
#endif
}
