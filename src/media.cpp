#include "media.h"
#include "app.h"
#include "ascii_pass.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_WINDOWS_UTF8 // SDL hands us UTF-8 paths, also on Windows
#include "stb_image.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

MediaKind g_kind = M_NONE;
bool g_playing = false;
double g_time = 0;
double g_duration = 0;
int g_frameShown = -1;

GLuint g_image = 0;
int g_mediaW = 0, g_mediaH = 0;
bool g_imageReady = false;

std::vector<unsigned char> g_gifPixels;
std::vector<double> g_gifStart;

void createPreview(int w, int h)
{
    if (!g_image) glGenTextures(1, &g_image);
    glBindTexture(GL_TEXTURE_2D, g_image);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    g_mediaW = w;
    g_mediaH = h;
}

void showFrame(const unsigned char* rgba)
{
    glBindTexture(GL_TEXTURE_2D, g_image);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, g_mediaW, g_mediaH, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    g_imageReady = true;
    renderAscii();
}

void startMedia(MediaKind kind, double duration)
{
    g_kind = kind;
    g_duration = duration;
    g_time = 0;
    g_playing = kind == M_GIF || kind == M_VIDEO;
    g_frameShown = -1;
    g_imageReady = false;
    g_lines.clear();
    g_colors.clear();
}

static bool loadImage(const std::string& path)
{
    int w, h, n;
    unsigned char* rgba = stbi_load(path.c_str(), &w, &h, &n, 4);
    if (!rgba) return false;

    stopVideo();
    startMedia(M_IMAGE, 0);
    createPreview(w, h);
    showFrame(rgba);
    stbi_image_free(rgba);
    return true;
}

static bool isGif(const std::string& path)
{
    char magic[4] = {};
    SDL_IOStream* io = SDL_IOFromFile(path.c_str(), "rb");
    if (!io) return false;
    SDL_ReadIO(io, magic, 4);
    SDL_CloseIO(io);
    return std::memcmp(magic, "GIF8", 4) == 0;
}

static bool loadGif(const std::string& path)
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
    createPreview(w, h);
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
    else if (g_kind == M_VIDEO)
    {
        updateVideo(dt);
    }
}
