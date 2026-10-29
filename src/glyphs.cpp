#include "glyphs.h"
#include "ascii_pass.h"
#include "gl.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include <algorithm>
#include <cmath>

Circle g_circles[N];
Circle g_extCircles[ECC];
std::vector<Glyph> g_glyphs;
static stbtt_fontinfo g_font; // glyph font for the shape vectors

void initGlyphFont(const unsigned char* ttf)
{
    stbtt_InitFont(&g_font, ttf, stbtt_GetFontOffsetForIndex(ttf, 0));
}

// ---------------------------------------------------------------------------
// Kd Tree
// ---------------------------------------------------------------------------
struct KdNode
{
    int glyph;   // index in g_glyphs
    int axis;    // which circle this node splits on
    int left = -1; int right = -1;
};

static std::vector<KdNode> g_kd;
static int g_KdRoot = -1;

// Build: sort the glyphs on one circle, the median becomes the node,
// smaller ones go left, bigger ones right. Next level uses the next circle.
static int buildKd(std::vector<int>& idx, int lo, int hi, int depth)
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

// Search: go down the side the cell is on, then only visit the other side
// if the dividing line is closer than the best glyph found so far.
static void KdNearest(int n, const Vec& in, int& best, float& bestDist)
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
static void layoutCircles(float radius, float stagger)
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
// Character shape vectors
// ---------------------------------------------------------------------------
static std::vector<Glyph> buildGlyphs(float fontScale)
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
    std::vector<Cell> vs = runPass(tex, cols, rows, (float)gw, (float)gh, 1, 1.0f);
    glDeleteTextures(1, &tex);

    std::vector<Glyph> glyphs;
    for (int i = 0; i < count; ++i)
    {
        Glyph gl;
        gl.ch = (char)(32 + i);
        gl.v = vs[(size_t)(2 * (i / perRow) + 1) * cols + 2 * (i % perRow) + 1].v;
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

void updateGlyphs(float radius, float stagger, float fontScale)
{
    layoutCircles(radius, stagger);
    g_glyphs = buildGlyphs(fontScale);
    std::vector<int> idx(g_glyphs.size());
    for (int i = 0; i < (int)idx.size(); i++) idx[i] = i;
    g_kd.clear();
    g_KdRoot = buildKd(idx, 0, (int)idx.size(), 0);
}
