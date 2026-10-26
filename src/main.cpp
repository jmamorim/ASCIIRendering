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

#include "app.h"
#include "ascii_pass.h"
#include "export.h"
#include "file_picker.h"
#include "gl.h"
#include "glyphs.h"
#include "media.h"
#include "params.h"
#include "ui.h"

#include "font_data.h" // DejaVu Sans Mono, embedded at build time (assets/)

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

SDL_Window* g_window = nullptr;
static SDL_GLContext g_gl = nullptr;

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------
static float g_dpi = 1;
static ImFont* g_uiFont = nullptr;
static bool g_done = false;

static void handleEvent(const SDL_Event& e)
{
    ImGui_ImplSDL3_ProcessEvent(&e);
    if (e.type == SDL_EVENT_QUIT) g_done = true;
    if (e.type == SDL_EVENT_DROP_FILE && !videoExportRunning()) openFile(e.drop.data);
}

// Everything after the events: open a picked file, advance playback, draw
static void runFrame()
{
    std::string picked = takePickedFile();
    if (!picked.empty() && !videoExportRunning()) openFile(picked);

    if (SDL_GetWindowFlags(g_window) & SDL_WINDOW_MINIMIZED) return;

    if (!exportVideoFrame()) updatePlayback();

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
    runPendingExport();
    SDL_GL_SwapWindow(g_window);
}

#ifdef __EMSCRIPTEN__
// The browser calls this once per display frame. It must return, or the page freezes.
static void webFrame()
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

    initGlyphFont(font_data);

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
        // While playing or exporting, don't sleep: vsync (or the export) paces the loop.
        bool minimized = SDL_GetWindowFlags(g_window) & SDL_WINDOW_MINIMIZED;
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, (g_playing || videoExportRunning()) && !minimized ? 0 : 100))
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
