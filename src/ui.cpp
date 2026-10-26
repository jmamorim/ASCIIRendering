#include "ui.h"
#include "ascii_pass.h"
#include "export.h"
#include "file_picker.h"
#include "glyphs.h"
#include "media.h"
#include "params.h"

#include <algorithm>
#include <cfloat>
#include <cstdint>

static const float PANEL_W = 300; // before DPI scaling

void onParamsChanged()
{
    // Glyph shape vectors only depend on these three, so only rebuild if they changed
    float r = val(P_RADIUS), s = val(P_STAGGER), f = val(P_FONT);
    static float lr = -1, ls = -1, lf = -1;
    if (r != lr || s != ls || f != lf)
    {
        updateGlyphs(r, s, f);
        lr = r; ls = s; lf = f;
    }

    renderAscii();
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------
static void paintAscii(ImFont* font)
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

static void paintPreview()
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
    bool exporting = videoExportRunning();
    if (exporting) showVideoExportProgress();
    ImGui::BeginDisabled(exporting); // everything else stays as it is until the export ends
    if (!exporting && (g_kind == M_GIF || g_kind == M_VIDEO))
    {
        ImGui::BeginDisabled(g_lines.empty());
        if (ImGui::Button("Export video...", ImVec2(-1, 0)))
            ImGui::OpenPopup("Export video");
        ImGui::EndDisabled();
    }
    else if (!exporting)
    {
        ImGui::BeginDisabled(g_kind != M_IMAGE || g_lines.empty());
        if (ImGui::Button("Export image...", ImVec2(-1, 0)))
            ImGui::OpenPopup("Export image");
        ImGui::EndDisabled();
    }
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
    ImGui::EndDisabled();
    ImGui::EndChild();

    // Right: the ASCII output
    ImGui::SameLine(0, 0);
    ImGui::BeginChild("ascii", ImVec2(0, 0), ImGuiChildFlags_None,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    paintAscii(font);
    ImGui::EndChild();

    ImGui::End();
}
