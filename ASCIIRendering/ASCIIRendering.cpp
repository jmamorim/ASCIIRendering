// Build (Developer Command Prompt for Visual Studio):
//   cl /EHsc ascii_ui.cpp
//
// Open an image, then drag the sliders to see the ASCII output change live.

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <gdiplus.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "Comdlg32.lib")
#pragma comment(lib, "Gdiplus.lib")
#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "User32.lib")
#pragma comment(lib, "Gdi32.lib")
#pragma comment(linker, "/SUBSYSTEM:WINDOWS")

using namespace Gdiplus;

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
    const wchar_t* name;
    int minv, maxv, init; // slider range (integers)
    float scale;          // real value = slider position * scale
    int decimals;         // decimals to show in the label
    HWND slider;
    HWND label;
};

Param P[P_COUNT] = {
    { L"Columns",         20, 250, 100, 1.00f, 0, nullptr, nullptr },
    { L"Sample step",      1,   8,   2, 1.00f, 0, nullptr, nullptr },
    { L"Contrast",        10,  40,  10, 0.10f, 1, nullptr, nullptr },
    { L"Circle radius",   15,  45,  30, 0.01f, 2, nullptr, nullptr },
    { L"Stagger",          0,  10,   4, 0.01f, 2, nullptr, nullptr },
    { L"Glyph font size", 50, 100,  80, 0.01f, 2, nullptr, nullptr },
};

float val(int i)
{
    return (float)SendMessageW(P[i].slider, TBM_GETPOS, 0, 0) * P[i].scale;
}

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
const int PANEL_W = 300;
const int ID_OPEN = 1001;
const int ID_COPY = 1002;
const int ID_SLIDER_BASE = 2000;

HWND g_hwnd = nullptr;
Bitmap* g_image = nullptr;           // for the preview
Gray g_gray;                         // grayscale copy for sampling
Circle g_circles[N];
std::vector<Glyph> g_glyphs;
std::vector<std::string> g_lines;    // current ASCII output

// ---------------------------------------------------------------------------
// File picker
// ---------------------------------------------------------------------------
std::wstring PickImageFile(HWND owner)
{
    OPENFILENAMEW ofn;
    WCHAR szFile[MAX_PATH] = { 0 };

    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = L"Image Files\0*.bmp;*.jpg;*.jpeg;*.png;*.gif\0All Files\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_EXPLORER;

    if (GetOpenFileNameW(&ofn))
        return std::wstring(szFile);
    return std::wstring();
}

// ---------------------------------------------------------------------------
// Bitmap -> grayscale buffer (LockBits = fast, reads the raw pixel memory)
// ---------------------------------------------------------------------------
Gray toGray(Bitmap& bmp)
{
    Gray g;
    int w = (int)bmp.GetWidth();
    int h = (int)bmp.GetHeight();

    BitmapData bd;
    Rect rect(0, 0, w, h);
    if (bmp.LockBits(&rect, ImageLockModeRead, PixelFormat32bppARGB, &bd) != Ok)
        return g;

    g.w = w;
    g.h = h;
    g.px.resize((size_t)w * h);

    for (int y = 0; y < h; ++y)
    {
        const BYTE* row = (const BYTE*)bd.Scan0 + (ptrdiff_t)y * bd.Stride;
        for (int x = 0; x < w; ++x)
        {
            // memory order is B, G, R, A
            float b = row[x * 4 + 0];
            float gg = row[x * 4 + 1];
            float r = row[x * 4 + 2];
            g.px[(size_t)y * w + x] = (0.2126f * r + 0.7152f * gg + 0.0722f * b) / 255.0f;
        }
    }

    bmp.UnlockBits(&bd);
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

    Bitmap bmp(gw, gh, PixelFormat32bppARGB);
    Graphics g(&bmp);
    g.SetTextRenderingHint(TextRenderingHintAntiAlias);

    FontFamily family(L"Consolas");
    Font font(&family, gh * fontScale, FontStyleRegular, UnitPixel);
    SolidBrush white(Color(255, 255, 255, 255));

    StringFormat fmt(StringFormat::GenericTypographic());
    fmt.SetAlignment(StringAlignmentCenter);
    fmt.SetLineAlignment(StringAlignmentCenter);
    RectF rect(0.0f, 0.0f, (REAL)gw, (REAL)gh);

    std::vector<Glyph> glyphs;

    for (char c = 32; c < 127; ++c)
    {
        g.Clear(Color(255, 0, 0, 0));
        WCHAR wc = (WCHAR)c;
        g.DrawString(&wc, 1, &font, rect, &fmt, &white);

        Gray gray = toGray(bmp);
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
RECT asciiArea()
{
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    rc.left = PANEL_W;
    return rc;
}

void updateLabels()
{
    for (int i = 0; i < P_COUNT; ++i)
    {
        wchar_t buf[64];
        swprintf_s(buf, L"%s: %.*f", P[i].name, P[i].decimals, val(i));
        SetWindowTextW(P[i].label, buf);
    }
}

void onParamsChanged()
{
    updateLabels();

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

    RECT area = asciiArea();
    InvalidateRect(g_hwnd, &area, FALSE);
}

void loadImage(const std::wstring& path)
{
    Bitmap* b = Bitmap::FromFile(path.c_str());
    if (!b || b->GetLastStatus() != Ok)
    {
        delete b;
        MessageBoxW(g_hwnd, L"Could not load the image.", L"Error", MB_ICONERROR);
        return;
    }

    delete g_image;
    g_image = b;
    g_gray = toGray(*g_image);

    onParamsChanged();
    InvalidateRect(g_hwnd, nullptr, FALSE); // repaint preview too
}

void copyToClipboard()
{
    if (g_lines.empty()) return;

    std::string text;
    for (const std::string& l : g_lines) { text += l; text += "\r\n"; }

    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1);
    if (!h) return;
    memcpy(GlobalLock(h), text.c_str(), text.size() + 1);
    GlobalUnlock(h);

    if (OpenClipboard(g_hwnd))
    {
        EmptyClipboard();
        SetClipboardData(CF_TEXT, h);
        CloseClipboard();
    }
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------
void paintAscii(HDC hdc, const RECT& area)
{
    int aw = area.right - area.left;
    int ah = area.bottom - area.top;
    if (aw <= 0 || ah <= 0) return;

    // Draw into an off-screen bitmap first so there is no flicker
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, aw, ah);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);

    RECT full = { 0, 0, aw, ah };
    FillRect(mem, &full, (HBRUSH)GetStockObject(BLACK_BRUSH));
    SetBkMode(mem, TRANSPARENT);

    if (g_lines.empty())
    {
        HGDIOBJ oldFont = SelectObject(mem, GetStockObject(DEFAULT_GUI_FONT));
        SetTextColor(mem, RGB(160, 160, 160));
        const wchar_t* hint = L"Click \"Open image...\" to start";
        TextOutW(mem, 20, 20, hint, (int)wcslen(hint));
        SelectObject(mem, oldFont);
    }
    else
    {
        int rows = (int)g_lines.size();
        int cols = (int)g_lines[0].size();

        // Largest character cell (width cell, height 2*cell) that fits the area
        int c1 = aw / cols;
        int c2 = ah / (rows * 2);
        int cell = c1 < c2 ? c1 : c2;
        if (cell < 2) cell = 2;

        HFONT font = CreateFontW(-(cell * 182 / 100), cell, 0, 0, FW_NORMAL, 0, 0, 0,
            ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
        HGDIOBJ oldFont = SelectObject(mem, font);
        SetTextColor(mem, RGB(255, 255, 255));

        for (int i = 0; i < rows; ++i)
            TextOutA(mem, 0, i * cell * 2, g_lines[i].c_str(), (int)g_lines[i].size());

        SelectObject(mem, oldFont);
        DeleteObject(font);
    }

    BitBlt(hdc, area.left, area.top, aw, ah, mem, 0, 0, SRCCOPY);

    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}

void paintPreview(HDC hdc, const RECT& client)
{
    if (!g_image) return;

    int x = 10;
    int y = 50 + P_COUNT * 54 + 10;
    int maxW = PANEL_W - 20;
    int maxH = client.bottom - y - 10;
    if (maxH < 20) return;

    float iw = (float)g_image->GetWidth();
    float ih = (float)g_image->GetHeight();
    float scale = maxW / iw;
    if (maxH / ih < scale) scale = maxH / ih;

    Graphics gfx(hdc);
    gfx.DrawImage(g_image, x, y, (INT)(iw * scale), (INT)(ih * scale));
}

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        g_hwnd = hwnd;
        HINSTANCE hInst = ((LPCREATESTRUCT)lParam)->hInstance;
        HFONT guiFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        HWND bOpen = CreateWindowExW(0, L"BUTTON", L"Open image...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            10, 10, 135, 28, hwnd, (HMENU)(INT_PTR)ID_OPEN, hInst, nullptr);
        HWND bCopy = CreateWindowExW(0, L"BUTTON", L"Copy text", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            155, 10, 135, 28, hwnd, (HMENU)(INT_PTR)ID_COPY, hInst, nullptr);
        SendMessageW(bOpen, WM_SETFONT, (WPARAM)guiFont, TRUE);
        SendMessageW(bCopy, WM_SETFONT, (WPARAM)guiFont, TRUE);

        for (int i = 0; i < P_COUNT; ++i)
        {
            int y = 50 + i * 54;

            P[i].label = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                10, y, 280, 18, hwnd, nullptr, hInst, nullptr);
            SendMessageW(P[i].label, WM_SETFONT, (WPARAM)guiFont, TRUE);

            P[i].slider = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
                WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
                10, y + 18, 280, 28, hwnd,
                (HMENU)(INT_PTR)(ID_SLIDER_BASE + i), hInst, nullptr);
            SendMessageW(P[i].slider, TBM_SETRANGE, TRUE, MAKELONG(P[i].minv, P[i].maxv));
            SendMessageW(P[i].slider, TBM_SETPOS, TRUE, P[i].init);
        }

        onParamsChanged(); // builds glyphs, fills labels
        return 0;
    }

    case WM_HSCROLL:
        onParamsChanged();
        return 0;

    case WM_COMMAND:
        if (LOWORD(wParam) == ID_OPEN)
        {
            std::wstring path = PickImageFile(hwnd);
            if (!path.empty()) loadImage(path);
        }
        else if (LOWORD(wParam) == ID_COPY)
        {
            copyToClipboard();
        }
        return 0;

    case WM_SIZE:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_ERASEBKGND:
        return 1; 

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        RECT client;
        GetClientRect(hwnd, &client);

        RECT panel = { 0, 0, PANEL_W, client.bottom };
        FillRect(hdc, &panel, (HBRUSH)(COLOR_BTNFACE + 1));
        paintPreview(hdc, client);

        RECT area = asciiArea();
        paintAscii(hdc, area);

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_DESTROY:
        delete g_image;
        g_image = nullptr;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nShow)
{
    GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken;
    GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, nullptr);

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"AsciiUiWindow";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"ASCII renderer",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 1200, 800,
        nullptr, nullptr, hInst, nullptr);
    ShowWindow(hwnd, nShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    GdiplusShutdown(gdiplusToken);
    return (int)msg.wParam;
}