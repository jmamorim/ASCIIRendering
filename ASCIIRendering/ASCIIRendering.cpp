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
#include "imgui_impl_opengl3.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_WINDOWS_UTF8 // SDL hands us UTF-8 paths, also on Windows
#include "stb_image.h"
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include "font_data.h" // DejaVu Sans Mono, embedded at build time (assets/)
#include "ascii_frag.h" // ascii.frag, embedded at build time

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
#include <emscripten/html5.h>
#include <GLES3/gl3.h>
#else
#include <atomic>
#include <condition_variable>
#include <thread>
#include <SDL3/SDL_opengl.h>
#endif

// ---------------------------------------------------------------------------
// OpenGL. The browser has every function; on desktop only GL 1.1 is linked,
// the rest is loaded at runtime.
// ---------------------------------------------------------------------------
#ifdef __EMSCRIPTEN__
const char* GLSL_VERSION = "#version 300 es";
#else
const char* GLSL_VERSION = "#version 330 core";

#define GL_FUNCS(X) \
    X(PFNGLCREATESHADERPROC, glCreateShader) \
    X(PFNGLSHADERSOURCEPROC, glShaderSource) \
    X(PFNGLCOMPILESHADERPROC, glCompileShader) \
    X(PFNGLGETSHADERIVPROC, glGetShaderiv) \
    X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog) \
    X(PFNGLDELETESHADERPROC, glDeleteShader) \
    X(PFNGLCREATEPROGRAMPROC, glCreateProgram) \
    X(PFNGLATTACHSHADERPROC, glAttachShader) \
    X(PFNGLLINKPROGRAMPROC, glLinkProgram) \
    X(PFNGLGETPROGRAMIVPROC, glGetProgramiv) \
    X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog) \
    X(PFNGLUSEPROGRAMPROC, glUseProgram) \
    X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation) \
    X(PFNGLUNIFORM1IPROC, glUniform1i) \
    X(PFNGLUNIFORM1FPROC, glUniform1f) \
    X(PFNGLUNIFORM2FPROC, glUniform2f) \
    X(PFNGLUNIFORM3FVPROC, glUniform3fv) \
    X(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers) \
    X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer) \
    X(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D) \
    X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus) \
    X(PFNGLDRAWBUFFERSPROC, glDrawBuffers) \
    X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays) \
    X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray)

#define X(type, name) static type name;
GL_FUNCS(X)
#undef X
#endif

bool loadGl()
{
#ifndef __EMSCRIPTEN__
#define X(type, name) if (!(name = (type)SDL_GL_GetProcAddress(#name))) return false;
    GL_FUNCS(X)
#undef X
#endif
    return true;
}

// ---------------------------------------------------------------------------
// Basic types
// ---------------------------------------------------------------------------
const int N = 6; // sampling circles per cell
using Vec = std::array<float, N>;

struct Circle { float cx, cy, r; }; // cx, cy: fraction of cell; r: fraction of cell width

const int ECC = 10; // external circles per cell
Circle g_extCircles[ECC];

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
SDL_GLContext g_gl = nullptr;
GLuint g_image = 0;                  // the preview, also the shader's input
int g_mediaW = 0, g_mediaH = 0;
bool g_imageReady = false;
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

struct KdNode
{
    int glyph;   // index in g_glyphs
    int axis;    // which circle this node splits on
    int left = -1; int right = -1;
};

std::vector<KdNode> g_kd;
int g_KdRoot = -1;

// Build: sort the glyphs on one circle, the median becomes the node,
// smaller ones go left, bigger ones right. Next level uses the next circle.
int buildKd(std::vector<int>& idx, int lo, int hi, int depth)
{
    if(lo >= hi) return -1;
    int axis = depth % N;
    int mid = (lo + hi) / 2;
    std::nth_element(idx.begin() + lo, idx.begin() + mid, idx.begin() + hi,
        [&] (int a, int b) {return g_glyphs[a].v[axis] < g_glyphs[b].v[axis];});
    int me = (int)g_kd.size();
    g_kd.push_back({ idx[mid], axis });
    int l = buildKd(idx, lo, mid, depth + 1);
    int r = buildKd(idx, mid + 1, hi, depth + 1);
    g_kd[me].left = l;
    g_kd[me].right = r;
    return me;
}

// ---------------------------------------------------------------------------
// Kd Tree 
// ---------------------------------------------------------------------------

// Search: go down the side the cell is on, then only visit the other side
// if the dividing line is closer than the best glyph found so far.
void KdNearest(int n, const Vec& in, int& best, float& bestDist)
{
    if(n < 0) return;
    const KdNode& node = g_kd[n];
    const Vec& p = g_glyphs[node.glyph].v;

    float d = 0.0f;
    for (int i = 0; i < N; i++)
    {
        float diff = p[i] - in[i];
        d += diff * diff;
    }
    if (d < bestDist)
    {
        bestDist = d;
        best = node.glyph;
    }

    //which side of the line is the cell on?
    float diff = in[node.axis] - p[node.axis];
    int nearSide = diff < 0 ? node.left : node.right;
    int farSide = diff < 0 ? node.right : node.left;

    KdNearest(nearSide, in, best, bestDist);
    if(diff * diff < bestDist)     // line closer than best : other side might win
        KdNearest(farSide, in, best, bestDist);
}

char findBestCharKd(const Vec& in)
{
    int best = 0;
    float bestDist = 1e30f;
    KdNearest(g_KdRoot, in, best, bestDist);
    return g_glyphs[best].ch;
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


// ---------------------------------------------------------------------------
// GPU pass: ascii.frag runs once per cell into a cols x rows target and
// writes the cell's 6 circle values; the glyph lookup stays on the CPU.
// ---------------------------------------------------------------------------
const char* FULLSCREEN_VS = R"(
void main()
{
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

struct AsciiPass
{
    GLuint prog = 0, vao = 0, fbo = 0;
    GLuint out[2] = {};     // circles 0..3, circles 4..5
    int cols = 0, rows = 0;
    GLint uImage, uCell, uCircles, uExt, uExponent, uStep;
    std::vector<float> v0123, v45;
};
AsciiPass g_pass;

GLuint compileShader(GLenum type, const char* body, int len)
{
    const char* src[] = { GLSL_VERSION, "\n", body };
    const GLint lens[] = { -1, -1, len };
    GLuint s = glCreateShader(type);
    glShaderSource(s, 3, src, lens);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[1024];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        SDL_Log("Shader compile failed: %s", log);
    }
    return s;
}

bool initAsciiPass()
{
    AsciiPass& p = g_pass;

    // Drop the file's own #version line, GLSL_VERSION replaces it
    const char* frag = (const char*)ascii_frag;
    const char* end = frag + sizeof(ascii_frag);
    const char* body = (const char*)std::memchr(frag, '\n', sizeof(ascii_frag)) + 1;

    GLuint vs = compileShader(GL_VERTEX_SHADER, FULLSCREEN_VS, -1);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, body, (int)(end - body));
    p.prog = glCreateProgram();
    glAttachShader(p.prog, vs);
    glAttachShader(p.prog, fs);
    glLinkProgram(p.prog);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok;
    glGetProgramiv(p.prog, GL_LINK_STATUS, &ok);
    if (!ok)
    {
        char log[1024];
        glGetProgramInfoLog(p.prog, sizeof(log), nullptr, log);
        SDL_Log("Shader link failed: %s", log);
        p.prog = 0;
        return false;
    }

    p.uImage = glGetUniformLocation(p.prog, "uImage");
    p.uCell = glGetUniformLocation(p.prog, "uCell");
    p.uCircles = glGetUniformLocation(p.prog, "uCircles");
    p.uExt = glGetUniformLocation(p.prog, "uExt");
    p.uExponent = glGetUniformLocation(p.prog, "uExponent");
    p.uStep = glGetUniformLocation(p.prog, "uStep");

    glGenVertexArrays(1, &p.vao);
    glGenFramebuffers(1, &p.fbo);
    glGenTextures(2, p.out);

#ifdef __EMSCRIPTEN__
    // WebGL 2 can only render to float textures with this
    emscripten_webgl_enable_extension(emscripten_webgl_get_current_context(), "EXT_color_buffer_float");
#endif
    return true;
}

void resizeTargets(int cols, int rows)
{
    AsciiPass& p = g_pass;
    if (cols == p.cols && rows == p.rows) return;

    glBindFramebuffer(GL_FRAMEBUFFER, p.fbo);
    for (int i = 0; i < 2; ++i)
    {
        glBindTexture(GL_TEXTURE_2D, p.out[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, cols, rows, 0, GL_RGBA, GL_FLOAT, nullptr);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, GL_TEXTURE_2D, p.out[i], 0);
    }
    const GLenum bufs[] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
    glDrawBuffers(2, bufs);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        SDL_Log("ASCII framebuffer is incomplete");

    p.cols = cols;
    p.rows = rows;
    p.v0123.resize((size_t)cols * rows * 4);
    p.v45.resize((size_t)cols * rows * 4);
}

std::vector<Vec> runPass(GLuint tex, int cols, int rows, float cw, float ch, int step, float exponent)
{
    AsciiPass& p = g_pass;
    resizeTargets(cols, rows);

    glBindFramebuffer(GL_FRAMEBUFFER, p.fbo);
    glViewport(0, 0, cols, rows);
    glUseProgram(p.prog);
    glBindTexture(GL_TEXTURE_2D, tex);
    glUniform1i(p.uImage, 0);
    glUniform2f(p.uCell, cw, ch);
    glUniform3fv(p.uCircles, N, &g_circles[0].cx);
    glUniform3fv(p.uExt, ECC, &g_extCircles[0].cx);
    glUniform1f(p.uExponent, exponent);
    glUniform1i(p.uStep, step);
    glBindVertexArray(p.vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glReadPixels(0, 0, cols, rows, GL_RGBA, GL_FLOAT, p.v0123.data());
    glReadBuffer(GL_COLOR_ATTACHMENT1);
    glReadPixels(0, 0, cols, rows, GL_RGBA, GL_FLOAT, p.v45.data());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    std::vector<Vec> out((size_t)cols * rows);
    for (size_t i = 0; i < out.size(); ++i)
    {
        const float* a = &p.v0123[i * 4];
        const float* b = &p.v45[i * 4];
        out[i] = { a[0], a[1], a[2], a[3], b[0], b[1] };
    }
    return out;
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

    // Glyphs on black in a grid, with an empty cell around each one:
    // circles reach past the cell edge and must not see a neighbouring glyph
    const int count = 127 - 32;
    const int perRow = 10;
    const int cols = 2 * perRow + 1;
    const int rows = 2 * ((count + perRow - 1) / perRow) + 1;
    const int aw = gw * cols, ah = gh * rows;
    std::vector<unsigned char> atlas((size_t)aw * ah * 4, 0);

    for (int i = 0; i < count; ++i)
    {
        char c = (char)(32 + i);
        int cx = (2 * (i % perRow) + 1) * gw, cy = (2 * (i / perRow) + 1) * gh;

        // Center the character's advance horizontally
        int advance, lsb;
        stbtt_GetCodepointHMetrics(&g_font, c, &advance, &lsb);
        float x = (gw - advance * scale) / 2;

        // Draw white text, opaque because gray() multiplies by alpha
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
                unsigned char* d = &atlas[((size_t)(cy + py) * aw + cx + px) * 4];
                d[0] = d[1] = d[2] = bmp[y * bw + xx];
            }
        }
        stbtt_FreeBitmap(bmp, nullptr);
    }
    for (size_t k = 3; k < atlas.size(); k += 4) atlas[k] = 255;

    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, aw, ah, 0, GL_RGBA, GL_UNSIGNED_BYTE, atlas.data());

    // Exponent 1 skips the contrast passes, glyphs are sampled as drawn
    std::vector<Vec> vs = runPass(tex, cols, rows, (float)gw, (float)gh, 1, 1.0f);
    glDeleteTextures(1, &tex);

    std::vector<Glyph> glyphs;
    for (int i = 0; i < count; ++i)
    {
        Glyph gl;
        gl.ch = (char)(32 + i);
        gl.v = vs[(size_t)(2 * (i / perRow) + 1) * cols + 2 * (i % perRow) + 1];
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

// ---------------------------------------------------------------------------
// Image -> lines of ASCII, using the current slider values
// ---------------------------------------------------------------------------
void renderAscii()
{
    g_lines.clear();
    if (!g_imageReady || !g_pass.prog) return;

    int cols = (int)(val(P_COLS) + 0.5f);
    int step = (int)(val(P_STEP) + 0.5f);
    float exponent = val(P_CONTRAST);

    float cw = (float)g_mediaW / cols;
    float ch = cw * 2.0f; // console characters are ~1:2
    int rows = (int)(g_mediaH / ch);
    if (rows <= 0) return;

    std::vector<Vec> cells = runPass(g_image, cols, rows, cw, ch, step, exponent);
    for (int r = 0; r < rows; ++r)
    {
        std::string line;
        for (int c = 0; c < cols; ++c)
            line += findBestCharKd(cells[(size_t)r * cols + c]);
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
        std::vector<int> idx(g_glyphs.size());
        for (int i = 0; i < (int)idx.size(); i++) idx[i] = i;
        g_kd.clear();
        g_KdRoot = buildKd(idx, 0, (int)idx.size(), 0);
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
bool g_playing = false;
double g_time = 0;      // playback position, seconds
double g_duration = 0;  // 0 = unknown
int g_frameShown = -1;  // index of the frame in g_image / g_lines

// GIF: every frame is decoded up front
std::vector<unsigned char> g_gifPixels; // RGBA, frame after frame
std::vector<double> g_gifStart;         // start time of each frame, seconds

std::vector<unsigned char> g_videoFrame; // the video frame the main thread is converting

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
    createPreview(w, h);
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
    createPreview(v.w, v.h);
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
    createPreview(w, h);
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
// Export the ASCII output as an image file
// ---------------------------------------------------------------------------
enum ExportFormat { E_PNG, E_JPG, E_BMP };
const char* EXPORT_FORMATS[] = { "PNG", "JPG", "BMP" };

int g_exportFormat = E_PNG;
int g_exportScale = 1;
ImVec2 g_asciiMin, g_asciiMax; // where paintAscii drew the characters, in window points
int g_exportIn = 0;            // frames until the screenshot, so the popup is gone first

#ifdef __EMSCRIPTEN__
// Browser: hand the bytes to the page as a download
EM_JS(void, jsDownload, (const char* name, const unsigned char* data, int size), {
    const a = document.createElement('a');
    a.href = URL.createObjectURL(new Blob([HEAPU8.slice(data, data + size)]));
    a.download = UTF8ToString(name);
    a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 1000);
});

void saveExport(const char* ext, std::vector<unsigned char> bytes)
{
    jsDownload((std::string("ascii.") + ext).c_str(), bytes.data(), (int)bytes.size());
}
#else
std::vector<unsigned char> g_exportBytes;
std::string g_exportExt;

void SDLCALL onSavePicked(void*, const char* const* files, int)
{
    if (!files || !files[0]) return; // error or cancelled
    std::string path = files[0];
    if (!path.ends_with("." + g_exportExt)) path += "." + g_exportExt;
    SDL_SaveFile(path.c_str(), g_exportBytes.data(), g_exportBytes.size());
}

void saveExport(const char* ext, std::vector<unsigned char> bytes)
{
    g_exportBytes = std::move(bytes);
    g_exportExt = ext;
    static SDL_DialogFileFilter filter;
    filter = { "Image", g_exportExt.c_str() };
    SDL_ShowSaveFileDialog(onSavePicked, nullptr, g_window, &filter, 1, "ascii");
}
#endif

// Screenshot of the ASCII area in the frame just drawn, so call it before the swap
void exportAsciiImage()
{
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 s = io.DisplayFramebufferScale;
    int x = (int)(g_asciiMin.x * s.x);
    int y = (int)((io.DisplaySize.y - g_asciiMax.y) * s.y); // GL counts rows from the bottom
    int w = (int)((g_asciiMax.x - g_asciiMin.x) * s.x);
    int h = (int)((g_asciiMax.y - g_asciiMin.y) * s.y);
    if (w <= 0 || h <= 0) return;

    std::vector<unsigned char> pixels((size_t)w * h * 4);
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

void showExportPopup()
{
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Export image", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;

    ImVec2 s = ImGui::GetIO().DisplayFramebufferScale;
    ImGui::Combo("Format", &g_exportFormat, EXPORT_FORMATS, IM_ARRAYSIZE(EXPORT_FORMATS));
    ImGui::SliderInt("Scale", &g_exportScale, 1, 4, "%dx");
    ImGui::Text("Output: %d x %d px", (int)((g_asciiMax.x - g_asciiMin.x) * s.x) * g_exportScale,
        (int)((g_asciiMax.y - g_asciiMin.y) * s.y) * g_exportScale);
    ImGui::Spacing();

    if (ImGui::Button("Export"))
    {
        g_exportIn = 2;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape))
        ImGui::CloseCurrentPopup();

    ImGui::EndPopup();
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

    g_asciiMin = p;
    g_asciiMax = ImVec2(p.x + (float)(cols * cell), p.y + (float)(rows * cell * 2));

    for (int i = 0; i < rows; ++i)
        dl->AddText(font, fontSize, ImVec2(p.x, p.y + (float)(i * cell * 2)),
            IM_COL32(255, 255, 255, 255), g_lines[i].c_str());
}

void paintPreview()
{
    if (!g_image) return;

    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.y < 20) return;

    float iw = (float)g_mediaW, ih = (float)g_mediaH;
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
    ImGui::BeginDisabled(g_kind != M_IMAGE || g_lines.empty());
    if (ImGui::Button("Export image...", ImVec2(-1, 0)))
        ImGui::OpenPopup("Export image");
    ImGui::EndDisabled();
    showExportPopup();
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

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    drawUi(g_dpi, g_uiFont);
    ImGui::Render();

    ImGuiIO& io = ImGui::GetIO();
    glViewport(0, 0, (int)(io.DisplaySize.x * io.DisplayFramebufferScale.x),
        (int)(io.DisplaySize.y * io.DisplayFramebufferScale.y));
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    if (g_exportIn > 0 && --g_exportIn == 0) exportAsciiImage();
    SDL_GL_SwapWindow(g_window);
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
    SDL_WindowFlags flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#ifdef __EMSCRIPTEN__
    flags |= SDL_WINDOW_FILL_DOCUMENT; // the canvas follows the browser window's size
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG); // macOS needs it
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
#endif
    g_window = SDL_CreateWindow("ASCII renderer", (int)(1200 * dpi), (int)(800 * dpi), flags);
    g_gl = g_window ? SDL_GL_CreateContext(g_window) : nullptr;
    if (!g_gl || !loadGl())
    {
        SDL_Log("Could not create the window: %s", SDL_GetError());
        return 1;
    }
    SDL_GL_MakeCurrent(g_window, g_gl);
#ifndef __EMSCRIPTEN__
    SDL_GL_SetSwapInterval(1); // the browser already paces frames to the display
#endif
    if (!initAsciiPass()) return 1;

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

    ImGui_ImplSDL3_InitForOpenGL(g_window, g_gl);
    ImGui_ImplOpenGL3_Init(GLSL_VERSION);

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
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DestroyContext(g_gl);
    SDL_DestroyWindow(g_window);
    SDL_Quit();
    return 0;
#endif
}
