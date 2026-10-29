#include "ascii_pass.h"
#include "media.h"
#include "params.h"

#include <SDL3/SDL.h>
#include <cmath>
#include <cstring>

#ifdef __EMSCRIPTEN__
#include <emscripten/html5.h>
#endif

#include "ascii_frag.h" // ascii.frag, embedded at build time

std::vector<std::string> g_lines;

static const char* FULLSCREEN_VS = R"(
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
static AsciiPass g_pass;

static GLuint compileShader(GLenum type, const char* body, int len)
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

static void resizeTargets(int cols, int rows)
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

std::vector<Cell> runPass(GLuint tex, int cols, int rows, float cw, float ch, int step, float exponent)
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

    std::vector<Cell> out((size_t)cols * rows);
    for (size_t i = 0; i < out.size(); ++i)
    {
        const float* a = &p.v0123[i * 4];
        const float* b = &p.v45[i * 4];
        out[i] = { {a[0], a[1], a[2], a[3], b[0], b[1]}, b[2], b[3] };
    }
    return out;
}

// Outline mode: a line character along the cell's edge, or a space when there is no edge
static char edgeChar(float strength, float angle)
{
    if (strength < val(P_EDGE)) return ' ';

    // The gradient points across the edge; the line runs 90° to it.
    // Characters repeat every 180°, so fold the angle into 4 bins of 45°.
    const float PI = 3.14159265f;
    float a = angle + PI / 2.0f;
    int bin = (int)std::lround(a / (PI / 4.0f)) & 3;
    return "-\\|/"[bin];
}

void renderAscii()
{
    g_lines.clear();
    if (!g_imageReady || !g_pass.prog) return;

    int cols = (int)(val(P_COLS) + 0.5f);
    int step = (int)(val(P_STEP) + 0.5f);
    float exponent = val(P_CONTRAST);
    bool outlineMode = val(P_EDGE) > 0.0f; // threshold 0 = off

    float cw = (float)g_mediaW / cols;
    float ch = cw * 2.0f; // console characters are ~1:2
    int rows = (int)(g_mediaH / ch);
    if (rows <= 0) return;

    std::vector<Cell> cells = runPass(g_image, cols, rows, cw, ch, step, exponent);
    for (int r = 0; r < rows; ++r)
    {
        std::string line;
        for (int c = 0; c < cols; ++c)
        {   
            const Cell& cell = cells[(size_t)r * cols + c]; 
            line += outlineMode ? edgeChar(cell.strength, cell.angle) : findBestCharKd(cell.v);
        }
        g_lines.push_back(line);
    }
}
