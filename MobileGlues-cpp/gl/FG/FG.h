#pragma once

#include <cstdlib>
#include <cstring>
#include <vector>

#ifndef __APPLE__
#include <malloc.h>
#endif

#ifdef __ANDROID__
#include <android/log.h>
#endif

#include "../../gles/gles.h"
#include "../../gles/loader.h"
#include "../../includes.h"
#include "../framebuffer.h"
#include "../glsl/glsl_for_es.h"
#include "../log.h"
#include "../mg.h"
#include "../pixel.h"
#include <GL/gl.h>
#include <glm/glm.hpp>

namespace FG_Context {
    extern GLuint g_prevFrameTex;
    extern GLuint g_currFrameTex;
    extern GLuint g_interpFrameTex;
    extern GLuint g_fgFBO;
    extern GLuint g_quadVAO;
    extern GLuint g_quadVBO;
    extern GLuint g_interpProgram;

    extern GLsizei g_width;
    extern GLsizei g_height;
    extern bool g_hasPrev;
    extern float g_lastConfidence;
}

extern bool fgInitialized;
void ApplyFG();
void InitFGResources();
