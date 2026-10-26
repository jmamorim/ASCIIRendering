#include "export.h"
#include "app.h"
#include "ascii_pass.h"
#include "encoder.h"
#include "gl.h"
#include "media.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <numeric>
#include <string>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

// ---------------------------------------------------------------------------
// Clipboard
// ---------------------------------------------------------------------------
#ifdef __EMSCRIPTEN__
// SDL keeps its clipboard to itself in the browser, so use the page's
EM_JS(void, setClipboardText, (const char* text), {
    navigator.clipboard.writeText(UTF8ToString(text)).catch(() => {});
});
#else
static void setClipboardText(const char* text)
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
// Export the ASCII output as an image file
// ---------------------------------------------------------------------------
enum ExportFormat { E_PNG, E_JPG, E_BMP };
static const char* EXPORT_FORMATS[] = { "PNG", "JPG", "BMP" };

static int g_exportFormat = E_PNG;
static int g_exportScale = 1;
ImVec2 g_asciiMin, g_asciiMax;
static int g_exportIn = 0;     // frames until the screenshot, so the popup is gone first

#ifdef __EMSCRIPTEN__
// Browser: hand the bytes to the page as a download
EM_JS(void, jsDownload, (const char* name, const unsigned char* data, int size), {
    const a = document.createElement('a');
    a.href = URL.createObjectURL(new Blob([HEAPU8.slice(data, data + size)]));
    a.download = UTF8ToString(name);
    a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 1000);
});

static void saveExport(const char* ext, std::vector<unsigned char> bytes)
{
    jsDownload((std::string("ascii.") + ext).c_str(), bytes.data(), (int)bytes.size());
}
#else
static std::vector<unsigned char> g_exportBytes;
static std::string g_exportExt;

static void SDLCALL onSavePicked(void*, const char* const* files, int)
{
    if (!files || !files[0]) return; // error or cancelled
    std::string path = files[0];
    if (!path.ends_with("." + g_exportExt)) path += "." + g_exportExt;
    SDL_SaveFile(path.c_str(), g_exportBytes.data(), g_exportBytes.size());
}

static void saveExport(const char* ext, std::vector<unsigned char> bytes)
{
    g_exportBytes = std::move(bytes);
    g_exportExt = ext;
    static SDL_DialogFileFilter filter;
    filter = { "Image", g_exportExt.c_str() };
    SDL_ShowSaveFileDialog(onSavePicked, nullptr, g_window, &filter, 1, "ascii");
}
#endif

bool readAsciiPixels(std::vector<unsigned char>& pixels, int& w, int& h)
{
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 s = io.DisplayFramebufferScale;
    int x = (int)(g_asciiMin.x * s.x);
    int y = (int)((io.DisplaySize.y - g_asciiMax.y) * s.y); // GL counts rows from the bottom
    w = (int)((g_asciiMax.x - g_asciiMin.x) * s.x);
    h = (int)((g_asciiMax.y - g_asciiMin.y) * s.y);
    if (w <= 0 || h <= 0) return false;

    pixels.resize((size_t)w * h * 4);
    glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    for (size_t k = 3; k < pixels.size(); k += 4) pixels[k] = 255;

    // Bigger image: repeat each pixel k x k times, which keeps the edges sharp instead of blurring
    int k = g_exportScale;
    if (k > 1)
    {
        std::vector<unsigned char> big((size_t)w * k * h * k * 4);
        for (int by = 0; by < h * k; ++by)
            for (int bx = 0; bx < w * k; ++bx)
                std::memcpy(&big[((size_t)by * w * k + bx) * 4], &pixels[((size_t)(by / k) * w + bx / k) * 4], 4);
        pixels.swap(big);
        w *= k;
        h *= k;
    }
    return true;
}

void exportAsciiImage()
{
    std::vector<unsigned char> pixels;
    int w, h;
    if (!readAsciiPixels(pixels, w, h)) return;

    std::vector<unsigned char> file;
    auto append = [](void* ctx, void* data, int size) {
        auto* out = (std::vector<unsigned char>*)ctx;
        out->insert(out->end(), (unsigned char*)data, (unsigned char*)data + size);
    };

    stbi_flip_vertically_on_write(1); // glReadPixels gives the bottom row first
    const char* ext = "png";
    switch (g_exportFormat)
    {
        case E_PNG: stbi_write_png_to_func(append, &file, w, h, 4, pixels.data(), w * 4); ext = "png"; break;
        case E_JPG: stbi_write_jpg_to_func(append, &file, w, h, 4, pixels.data(), 90); ext = "jpg"; break;
        case E_BMP: stbi_write_bmp_to_func(append, &file, w, h, 4, pixels.data()); ext = "bmp"; break;
    }

    saveExport(ext, std::move(file));
}

// ---------------------------------------------------------------------------
// Export the ASCII output of a GIF or video as a video file
// ---------------------------------------------------------------------------
// Each loop pass shows the next source frame before the UI is drawn
// (exportVideoFrame), then screenshots it after (runPendingExport) and hands it to
// the encoder. The playback clock is ignored, so no frame is skipped.
static const char* VIDEO_FORMATS[] = { "MP4", "GIF" };
static const char* VIDEO_EXTS[] = { "mp4", "gif" };
static int g_videoFormat = V_MP4;

static struct
{
    enum { IDLE, PICKING, RUNNING, FINISHING } state = IDLE;
    std::string path;
    std::string rate;          // output fps, as ffmpeg reads it ("30000/1001")
    double fps = 0;
    int frame = 0;             // next frame to encode
    int total = 0;             // 0 = unknown
    bool capture = false;      // a frame was shown this pass, screenshot it
    bool opened = false;       // the encoder has been started (on the first frame)
    int w = 0, h = 0;
    std::vector<unsigned char> pixels;
    double savedTime = 0;
    bool wasPlaying = false;
} E;

// Back to normal playback where it was before the export. Unless `done`, the
// encoder is stopped and its output thrown away.
static void endVideoExport(const char* error, bool done = false)
{
    if (!done) encoderAbort();
    E.state = E.IDLE;
    E.pixels = {};
    SDL_GL_SetSwapInterval(1);

    g_time = E.savedTime;
    g_frameShown = -1;
    if (g_kind == M_VIDEO) startVideo(g_time);
    g_playing = E.wasPlaying;

    if (error) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Export failed", error, g_window);
}

static void beginVideoExport(std::string path)
{
    std::string ext = std::string(".") + VIDEO_EXTS[g_videoFormat];
    if (!path.ends_with(ext)) path += ext;
    E.path = path;
    E.frame = 0;
    E.capture = false;
    E.opened = false;
    E.savedTime = g_time;
    E.wasPlaying = g_playing;
    g_playing = false;
    E.state = E.RUNNING;
    encoderPrepare((VideoFormat)g_videoFormat);

    if (g_kind == M_VIDEO)
    {
        E.rate = videoRate();
        E.fps = videoFps();
        if (!startVideo(0)) return endVideoExport("Could not restart reading the video.");
    }
    else
    {
        // GIF delays are whole milliseconds (in practice tens of them), so a rate of
        // 1000 / gcd(delays) frames per second lands on every frame change
        int g = 0;
        for (size_t i = 0; i < g_gifStart.size(); ++i)
        {
            double end = i + 1 < g_gifStart.size() ? g_gifStart[i + 1] : g_duration;
            g = std::gcd(g, (int)std::lround((end - g_gifStart[i]) * 1000));
        }
        g = std::max(g, 10); // at most 100 fps
        E.rate = "1000/" + std::to_string(g);
        E.fps = 1000.0 / g;
    }
    E.total = (int)std::lround(g_duration * E.fps);
    SDL_GL_SetSwapInterval(0); // export as fast as frames convert, not at the display rate
}

#ifdef __EMSCRIPTEN__
static void pickVideoPath()
{
    beginVideoExport("ascii"); // the browser downloads it under this name
}
#else
// The save dialog may answer on another thread
static std::mutex g_savePathMutex;
static std::string g_savePath;
static bool g_saveAnswered = false;

static void SDLCALL onVideoSavePicked(void*, const char* const* files, int)
{
    std::lock_guard<std::mutex> lock(g_savePathMutex);
    g_savePath = files && files[0] ? files[0] : ""; // "" = error or cancelled
    g_saveAnswered = true;
}

static void pickVideoPath()
{
    {
        std::lock_guard<std::mutex> lock(g_savePathMutex);
        g_saveAnswered = false;
    }
    E.state = E.PICKING;
    static SDL_DialogFileFilter filter;
    filter = { "Video", VIDEO_EXTS[g_videoFormat] };
    SDL_ShowSaveFileDialog(onVideoSavePicked, nullptr, g_window, &filter, 1, "ascii");
}

static void checkPickedPath()
{
    std::lock_guard<std::mutex> lock(g_savePathMutex);
    if (!g_saveAnswered) return;
    E.state = E.IDLE;
    if (!g_savePath.empty()) beginVideoExport(g_savePath);
}
#endif

static void captureVideoFrame()
{
    int w, h;
    if (!readAsciiPixels(E.pixels, w, h)) return endVideoExport("The ASCII output is empty.");
    if (!E.opened)
    {
        if (!encoderOpen((VideoFormat)g_videoFormat, E.path, w, h, E.rate, E.fps))
            return endVideoExport(encoderError().c_str());
        E.opened = true;
        E.w = w;
        E.h = h;
    }
    else if (w != E.w || h != E.h)
    {
        return endVideoExport("The window changed size during the export.");
    }
    if (!encoderWrite(E.pixels)) return endVideoExport(encoderError().c_str());
    ++E.frame;
}

bool videoExportRunning()
{
    return E.state == E.RUNNING || E.state == E.FINISHING;
}

bool exportVideoFrame()
{
#ifndef __EMSCRIPTEN__
    if (E.state == E.PICKING) checkPickedPath();
#endif
    if (!videoExportRunning()) return false;

    int status = encoderPoll();
    if (status < 0) endVideoExport(encoderError().c_str());
    else if (status > 0) endVideoExport(nullptr, true);
    if (E.state != E.RUNNING || encoderBusy()) return true;

    // 1 = shown, 0 = not ready yet (try again next pass), -1 = past the end
    int shown;
    if (g_kind == M_VIDEO)
    {
        shown = showVideoFrame(E.frame);
        if (shown > 0) g_time = E.frame / E.fps;
    }
    else
    {
        shown = E.frame < E.total ? 1 : -1;
        if (shown > 0)
        {
            g_time = (E.frame + 0.5) / E.fps; // mid-frame, clear of rounding at frame changes
            updatePlayback();                 // not playing, so this only shows the frame at g_time
        }
    }

    if (shown > 0)
    {
        E.capture = true;
    }
    else if (shown < 0)
    {
        if (!E.opened)
        {
            endVideoExport("There were no frames to export.");
            return true;
        }
        encoderClose(); // the encoder finishes the file in the background; encoderPoll says when
        E.state = E.FINISHING;
    }
    return true;
}

void showVideoExportProgress()
{
    char text[64];
    if (E.state == E.FINISHING)
        SDL_snprintf(text, sizeof text, "Writing the file...");
    else if (E.total > 0)
        SDL_snprintf(text, sizeof text, "Frame %d / %d", E.frame, E.total);
    else
        SDL_snprintf(text, sizeof text, "Frame %d", E.frame);
    float done = E.state == E.FINISHING ? 1.0f : E.total > 0 ? (float)E.frame / E.total : 0.0f;
    ImGui::ProgressBar(std::min(done, 1.0f), ImVec2(-1, 0), text);
    if (ImGui::Button("Cancel export", ImVec2(-1, 0))) endVideoExport(nullptr);
}

// Format and scale; true when Export was pressed
static bool exportPopup(const char* title, int* format, const char* const* names, int count)
{
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return false;

    ImVec2 s = ImGui::GetIO().DisplayFramebufferScale;
    ImGui::Combo("Format", format, names, count);
    ImGui::SliderInt("Scale", &g_exportScale, 1, 4, "%dx");
    ImGui::Text("Output: %d x %d px", (int)((g_asciiMax.x - g_asciiMin.x) * s.x) * g_exportScale,
        (int)((g_asciiMax.y - g_asciiMin.y) * s.y) * g_exportScale);
    ImGui::Spacing();

    bool go = ImGui::Button("Export");
    if (go) ImGui::CloseCurrentPopup();
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape))
        ImGui::CloseCurrentPopup();

    ImGui::EndPopup();
    return go;
}

void showExportPopup()
{
    if (exportPopup("Export image", &g_exportFormat, EXPORT_FORMATS, IM_ARRAYSIZE(EXPORT_FORMATS)))
        g_exportIn = 2;
    if (exportPopup("Export video", &g_videoFormat, VIDEO_FORMATS, IM_ARRAYSIZE(VIDEO_FORMATS)))
        pickVideoPath();
}

void runPendingExport()
{
    if (g_exportIn > 0 && --g_exportIn == 0) exportAsciiImage();
    if (E.state == E.RUNNING && E.capture)
    {
        E.capture = false;
        captureVideoFrame();
    }
}
