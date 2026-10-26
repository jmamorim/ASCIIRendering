#include "gl.h"

#include <SDL3/SDL.h>

#ifdef __EMSCRIPTEN__
const char* GLSL_VERSION = "#version 300 es";
#else
const char* GLSL_VERSION = "#version 330 core";

namespace glfn
{
#define X(type, name) type name;
GL_FUNCS(X)
#undef X
}
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
