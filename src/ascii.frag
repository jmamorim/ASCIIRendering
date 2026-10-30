#version 300 es

precision highp float;

// ---------------------------------------------------------------------------
// Inputs
// ---------------------------------------------------------------------------
const int N   = 6;  // sampling circles per cell
const int ECC = 10; // external circles per cell
const int AFFECTING[N * 4] = int[](
    0, 1, 2, 4,
    0, 1, 3, 5,
    2, 4, 6, -1,
    3, 5, 7, -1,
    4, 6, 8, 9,
    5, 7, 8, 9);

uniform sampler2D uImage;      // the RGBA image, uploaded as-is
uniform vec2  uCell;           // cw, ch in image pixels
uniform vec3  uCircles[N];     // cx, cy, r  (same values as g_circles)
uniform vec3  uExt[ECC];       // g_extCircles
uniform float uExponent;       // P_CONTRAST
uniform int   uStep;           // P_STEP

// The 6 circle values of this cell, read back and matched on the CPU (findBestCharKd).
// Two render targets so each cell is sampled once.
layout(location = 0) out vec4 outV0123; // circles 0..3
layout(location = 1) out vec4 outV45;   // circles 4..5 in .xy
layout(location = 2) out vec4 outColor; // average rgb of cell

// ---------------------------------------------------------------------------
// RGBA pixel -> lightness (toGray)
// ---------------------------------------------------------------------------
float gray(ivec2 p)
{
    vec4 c = texelFetch(uImage, p, 0);
    return dot(c.rgb, vec3(0.2126, 0.7152, 0.0722)) * c.a; // transparent -> 0
}

// ---------------------------------------------------------------------------
// Brightness change at one pixel: x = left->right, y = top->bottom
// ---------------------------------------------------------------------------

float grayClamped(ivec2 p){
    return gray(clamp(p, ivec2(0), textureSize(uImage,0) - 1));
} 

vec2 sobel(ivec2 p)
{
    // the 3x3 neighbourhood:  a b c
    //                         d . f
    //                         g h i
    float a = grayClamped(p + ivec2(-1, -1));
    float b = grayClamped(p + ivec2( 0, -1));
    float c = grayClamped(p + ivec2( 1, -1));
    float d = grayClamped(p + ivec2(-1,  0));
    float f = grayClamped(p + ivec2( 1,  0));
    float g = grayClamped(p + ivec2(-1,  1));
    float h = grayClamped(p + ivec2( 0,  1));
    float i = grayClamped(p + ivec2( 1,  1));

    float gx = (c + 2.0 * f + i) - (a + 2.0 * d + g); // right column - left column
    float gy = (g + 2.0 * h + i) - (a + 2.0 * b + c); // bottom row - top row
    return vec2(gx, gy);
}

// Average gradient of the cell; vectors are summed, so opposite edges cancel
vec2 cellGradient(vec2 origin, out vec3 avgRgb)
{
    ivec2 lo = ivec2(origin);
    ivec2 hi = ivec2(origin + uCell);

    vec2 sum = vec2(0.0);
    vec3 sumRGB = vec3(0.0);
    int count = 0;
    for(int y = lo.y; y < hi.y; y += uStep)
        for(int x = lo.x; x < hi.x; x += uStep)
        {
            sum += sobel(ivec2(x,y));
            vec4 c = texelFetch(uImage, ivec2(x, y), 0);
            sumRGB += c.rgb * c.a;
            ++count;
        }
    avgRgb = count > 0 ? sumRGB / float(count) : vec3(0.0);
    return count > 0 ? sum / float(count) : vec2(0.0);
}

// ---------------------------------------------------------------------------
// Average lightness of the pixels inside one circle (sampleCircle)
// ---------------------------------------------------------------------------
float sampleCircle(vec2 origin, vec3 circ)
{
    vec2  c = origin + circ.xy * uCell;
    float r = circ.z * uCell.x;
    ivec2 size = textureSize(uImage, 0);

    ivec2 lo = ivec2(floor(c - r));
    ivec2 hi = ivec2(ceil(c + r));

    float sum = 0.0;
    int count = 0;

    for (int y = lo.y; y <= hi.y; y += uStep)
    {
        for (int x = lo.x; x <= hi.x; x += uStep)
        {
            if (x < 0 || y < 0 || x >= size.x || y >= size.y) continue;

            vec2 d = vec2(x, y) + 0.5 - c;
            if (dot(d, d) > r * r) continue; // outside the circle

            sum += gray(ivec2(x, y));
            ++count;
        }
    }

    if (count > 0) return sum / float(count);

    // Circle was tiny or the step skipped everything: fall back to the center pixel
    ivec2 p = ivec2(c);
    if (all(greaterThanEqual(p, ivec2(0))) && all(lessThan(p, size))) return gray(p);
    return 0.0;
}


// ---------------------------------------------------------------------------
// One character cell (the body of the loop in renderAscii)
// ---------------------------------------------------------------------------
void main()
{
    ivec2 cell = ivec2(gl_FragCoord.xy); // (c, r)
    vec2 origin = vec2(cell) * uCell;    // (c * cw, r * ch)

    float v[N];
    for (int i = 0; i < N; ++i)
        v[i] = sampleCircle(origin, uCircles[i]);

    if (uExponent > 1.001)
    {
        // sampleExternal
        float e[ECC];
        for (int i = 0; i < ECC; i++)
        {
            e[i] = sampleCircle(origin, uExt[i]);
        }

        // enhanceDirectional
        // rows
        for (int i = 0; i < N; i++)
        {
            float m = v[i];
            // collums thats why its 4
            for (int j = 0; j < 4; j++)
            { 
                int k = AFFECTING[i * 4 + j];
                if (k >= 0) m = max(m, e[k]);
            }
            if(m > 0.0f) v[i] = pow(v[i] / m, uExponent) * m;
        }

        // enhanceContrast
        float m = v[0];
        for(int i = 1; i < N; i++)
            m = max(m, v[i]);
        
        if(m > 0.0f)
        {
            for(int i = 0; i < N; i++)
                v[i] = pow(v[i] / m, uExponent) * m;
        }
    }

    outV0123 = vec4(v[0], v[1], v[2], v[3]);
    vec3 avgRgb;
    vec2 grad = cellGradient(origin, avgRgb);
    outV45   = vec4(v[4], v[5], length(grad), atan(grad.y, grad.x));
    outColor = vec4(avgRgb, 1.0);
}
