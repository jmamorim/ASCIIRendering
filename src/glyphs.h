// Sampling circles, character shape vectors and the kd-tree that finds the
// character whose shape is closest to a cell's
#pragma once

#include <array>
#include <vector>

const int N = 6; // sampling circles per cell
using Vec = std::array<float, N>;

struct Circle { float cx, cy, r; }; // cx, cy: fraction of cell; r: fraction of cell width

const int ECC = 10; // external circles per cell

struct Glyph
{
    char ch;
    Vec v;
};

extern Circle g_circles[N];
extern Circle g_extCircles[ECC];
extern std::vector<Glyph> g_glyphs;

// Load the glyph font (a TrueType file in memory), once at startup
void initGlyphFont(const unsigned char* ttf);

// Lay out the circles and rebuild the shape vectors and the kd-tree
void updateGlyphs(float radius, float stagger, float fontScale);

char findBestCharKd(const Vec& in);
