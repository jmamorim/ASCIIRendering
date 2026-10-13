// Build (Windows, macOS, Linux): see README.md
//   cmake -S . -B build && cmake --build build
//
// Open an image, then drag the sliders to see the ASCII output change live.

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
#include <mutex>
#include <string>
#include <vector>

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

void SDLCALL onFilePicked(void*, const char* const* files, int)
{
    if (!files || !files[0]) return; // error or cancelled
    std::lock_guard<std::mutex> lock(g_pickedMutex);
    g_picked = files[0];
}

void PickImageFile()
{
    static const SDL_DialogFileFilter filters[] = {
        { "Image Files", "bmp;jpg;jpeg;png;gif;tga;psd" },
        { "All Files", "*" },
    };
    SDL_ShowOpenFileDialog(onFilePicked, nullptr, g_window, filters, 2, nullptr, false);
}

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
            g.px[(size_t)y * w + x] = (0.2126f * r + 0.7152f * gg + 0.0722f * b) / 255.0f;
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
            if (exponent > 1.001f) enhanceContrast(v, exponent);
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

void loadImage(const std::string& path)
{
    int w, h, n;
    unsigned char* rgba = stbi_load(path.c_str(), &w, &h, &n, 4);
    if (!rgba)
    {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Error", "Could not load the image.", g_window);
        return;
    }

    SDL_Texture* tex = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, w, h);
    if (tex) SDL_UpdateTexture(tex, nullptr, rgba, w * 4);

    SDL_DestroyTexture(g_image);
    g_image = tex;
    g_gray = toGray(rgba, w, h);
    stbi_image_free(rgba);

    onParamsChanged();
}

void copyToClipboard()
{
    if (g_lines.empty()) return;

    std::string text;
    for (const std::string& l : g_lines) { text += l; text += "\n"; }
    SDL_SetClipboardText(text.c_str());
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
            "Click \"Open image...\" or drop an image here to start");
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
    if (ImGui::Button("Open image...", ImVec2(half, 0)))
        PickImageFile();
    ImGui::SameLine();
    if (ImGui::Button("Copy text", ImVec2(half, 0)))
        copyToClipboard();
    ImGui::Spacing();

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
    g_window = SDL_CreateWindow("ASCII renderer", (int)(1200 * dpi), (int)(800 * dpi),
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    g_renderer = g_window ? SDL_CreateRenderer(g_window, nullptr) : nullptr;
    if (!g_renderer)
    {
        SDL_Log("Could not create the window: %s", SDL_GetError());
        return 1;
    }
    SDL_SetRenderVSync(g_renderer, 1);

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
    ImFont* font = io.Fonts->AddFontFromMemoryTTF((void*)font_data, (int)sizeof(font_data), 15.0f, &cfg);

    ImGui_ImplSDL3_InitForSDLRenderer(g_window, g_renderer);
    ImGui_ImplSDLRenderer3_Init(g_renderer);

    for (Param& p : P) p.pos = p.init;
    onParamsChanged(); // builds glyphs
    if (argc > 1) loadImage(argv[1]);

    bool done = false;
    while (!done)
    {
        // Sleep until something happens; the timeout picks up file dialog results
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 100))
        {
            do
            {
                ImGui_ImplSDL3_ProcessEvent(&e);
                if (e.type == SDL_EVENT_QUIT) done = true;
                if (e.type == SDL_EVENT_DROP_FILE) loadImage(e.drop.data);
            } while (SDL_PollEvent(&e));
        }

        std::string picked;
        {
            std::lock_guard<std::mutex> lock(g_pickedMutex);
            picked.swap(g_picked);
        }
        if (!picked.empty()) loadImage(picked);

        if (SDL_GetWindowFlags(g_window) & SDL_WINDOW_MINIMIZED) continue;

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        drawUi(dpi, font);
        ImGui::Render();

        SDL_SetRenderScale(g_renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
        SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
        SDL_RenderClear(g_renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), g_renderer);
        SDL_RenderPresent(g_renderer);
    }

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyTexture(g_image);
    SDL_DestroyRenderer(g_renderer);
    SDL_DestroyWindow(g_window);
    SDL_Quit();
    return 0;
}
