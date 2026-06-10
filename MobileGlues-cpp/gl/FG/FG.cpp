#include "FG.h"
#include "../../config/settings.h"

#define DEBUG 0

// Vertex shader (shared)
static const char* FSR3FG_VSSource = R"(
#version 100
attribute vec2 aPos;
attribute vec2 aTexCoord;
varying vec2 vTexCoord;
void main() {
    gl_Position = vec4(aPos.x, aPos.y, 0.0, 1.0);
    vTexCoord = aTexCoord;
}
)";

// Luminance extraction: RGB to luma using BT.709 coefficients
static const char* FSR3FG_LumaFSSource = R"(
#version 100
precision highp float;
varying vec2 vTexCoord;
uniform sampler2D uInputTex;
void main() {
    vec4 color = texture2D(uInputTex, vTexCoord);
    float luma = dot(color.rgb, vec3(0.2126, 0.7152, 0.0722));
    gl_FragColor = vec4(luma);
}
)";

// Hierarchical optical flow estimation
// 5x5 pixel search, 3x3 patch SAD, sub-pixel refinement, confidence output
static const char* FSR3FG_FlowFSSource = R"(
#version 100
precision highp float;
varying vec2 vTexCoord;
uniform sampler2D uPrevLuma;
uniform sampler2D uCurrLuma;
uniform vec2 uTexelSize;

void main() {
    vec2 uv = vTexCoord;
    float bestSad = 1e10;
    vec2 bestFlow = vec2(0.0);

    for (int dy = -2; dy <= 2; dy++) {
        for (int dx = -2; dx <= 2; dx++) {
            vec2 flowPx = vec2(float(dx), float(dy));
            vec2 flowUv = flowPx * uTexelSize;
            float sad = 0.0;
            for (int py = -1; py <= 1; py++) {
                for (int px = -1; px <= 1; px++) {
                    vec2 po = vec2(float(px), float(py)) * uTexelSize;
                    float pv = texture2D(uPrevLuma, uv + flowUv + po).r;
                    float cv = texture2D(uCurrLuma, uv + po).r;
                    sad += abs(pv - cv);
                }
            }
            if (sad < bestSad) {
                bestSad = sad;
                bestFlow = flowPx;
            }
        }
    }

    // Sub-pixel refinement via 1D parabola fitting along best axis
    // Test neighbors at +1 and -1 pixel on each axis
    vec2 refineFlow = bestFlow;
    float refineSad = bestSad;

    // X-axis refinement
    if (bestFlow.x > -2.0 && bestFlow.x < 2.0) {
        vec2 testPx = bestFlow;
        float sadM1 = 1e10, sadP1 = 1e10;
        testPx.x = bestFlow.x - 1.0;
        vec2 flowUvM1 = testPx * uTexelSize;
        float sadM1Acc = 0.0;
        for (int py = -1; py <= 1; py++) {
            for (int px = -1; px <= 1; px++) {
                vec2 po = vec2(float(px), float(py)) * uTexelSize;
                float pv = texture2D(uPrevLuma, uv + flowUvM1 + po).r;
                float cv = texture2D(uCurrLuma, uv + po).r;
                sadM1Acc += abs(pv - cv);
            }
        }
        sadM1 = sadM1Acc;

        testPx.x = bestFlow.x + 1.0;
        vec2 flowUvP1 = testPx * uTexelSize;
        float sadP1Acc = 0.0;
        for (int py = -1; py <= 1; py++) {
            for (int px = -1; px <= 1; px++) {
                vec2 po = vec2(float(px), float(py)) * uTexelSize;
                float pv = texture2D(uPrevLuma, uv + flowUvP1 + po).r;
                float cv = texture2D(uCurrLuma, uv + po).r;
                sadP1Acc += abs(pv - cv);
            }
        }
        sadP1 = sadP1Acc;

        if (sadM1 < refineSad || sadP1 < refineSad) {
            float a = sadM1;
            float b = refineSad;
            float c = sadP1;
            float denom = a - 2.0 * b + c;
            if (abs(denom) > 0.001) {
                float subPx = (a - c) / (2.0 * denom);
                refineFlow.x = bestFlow.x + subPx;
                if (sadM1 < refineSad) refineSad = sadM1;
                if (sadP1 < refineSad) refineSad = sadP1;
            }
        }
    }

    // Y-axis refinement
    if (bestFlow.y > -2.0 && bestFlow.y < 2.0) {
        vec2 testPx = bestFlow;
        float sadM1 = 1e10, sadP1 = 1e10;
        testPx.y = bestFlow.y - 1.0;
        vec2 flowUvM1 = testPx * uTexelSize;
        float sadM1Acc = 0.0;
        for (int py = -1; py <= 1; py++) {
            for (int px = -1; px <= 1; px++) {
                vec2 po = vec2(float(px), float(py)) * uTexelSize;
                float pv = texture2D(uPrevLuma, uv + flowUvM1 + po).r;
                float cv = texture2D(uCurrLuma, uv + po).r;
                sadM1Acc += abs(pv - cv);
            }
        }
        sadM1 = sadM1Acc;

        testPx.y = bestFlow.y + 1.0;
        vec2 flowUvP1 = testPx * uTexelSize;
        float sadP1Acc = 0.0;
        for (int py = -1; py <= 1; py++) {
            for (int px = -1; px <= 1; px++) {
                vec2 po = vec2(float(px), float(py)) * uTexelSize;
                float pv = texture2D(uPrevLuma, uv + flowUvP1 + po).r;
                float cv = texture2D(uCurrLuma, uv + po).r;
                sadP1Acc += abs(pv - cv);
            }
        }
        sadP1 = sadP1Acc;

        if (sadM1 < refineSad || sadP1 < refineSad) {
            float a = sadM1;
            float b = refineSad;
            float c = sadP1;
            float denom = a - 2.0 * b + c;
            if (abs(denom) > 0.001) {
                float subPx = (a - c) / (2.0 * denom);
                refineFlow.y = bestFlow.y + subPx;
            }
        }
    }

    bestFlow = refineFlow;

    // Encode flow: [-2, 2] pixels -> [0, 1]
    vec2 encodedFlow = (bestFlow + vec2(2.0)) / 4.0;
    float confidence = 1.0 - smoothstep(0.3, 3.0, bestSad / 9.0);

    gl_FragColor = vec4(encodedFlow, confidence, 1.0);
}
)";

// Frame interpolation using optical flow
// Asymmetric warping: prev at -flow/2, curr at +flow/2
static const char* FSR3FG_InterpFSSource = R"(
#version 100
precision highp float;
varying vec2 vTexCoord;
uniform sampler2D uPrevFrame;
uniform sampler2D uCurrFrame;
uniform sampler2D uFlowTex;
uniform vec2 uTexelSize;

void main() {
    vec2 uv = vTexCoord;
    vec4 flowEncoded = texture2D(uFlowTex, uv);

    // Decode flow from [0,1] to [-2, 2] pixels
    vec2 flowPixels = flowEncoded.rg * 4.0 - 2.0;
    float confidence = flowEncoded.b;

    vec2 flowUV = flowPixels * uTexelSize;

    // Asymmetric midpoint warping
    vec2 prevUV = uv - flowUV * 0.5;
    vec2 currUV = uv + flowUV * 0.5;

    vec4 prevColor = texture2D(uPrevFrame, prevUV);
    vec4 currColor = texture2D(uCurrFrame, currUV);

    // Adaptive blending: higher flow -> favor current frame to reduce ghosting
    float flowMag = length(flowPixels);
    float blendFactor = 0.5 + flowMag * 0.05;
    blendFactor = clamp(blendFactor, 0.3, 0.7);

    // Reduce blend confidence for unreliable flow
    blendFactor = mix(blendFactor, 0.7, 1.0 - confidence);

    gl_FragColor = mix(prevColor, currColor, blendFactor);
}
)";

struct GLStateGuard {
    GLint prevProgram;
    GLint prevVAO;
    GLint prevArrayBuffer;
    GLint prevActiveTexture;
    GLint prevTexture[2];
    GLint prevReadFBO;
    GLint prevDrawFBO;
    GLint prevRenderbuffer;
    GLint prevViewport[4];

    GLStateGuard() {
        GLES.glGetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);
        GLES.glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVAO);
        GLES.glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevArrayBuffer);
        GLES.glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActiveTexture);
        GLES.glActiveTexture(GL_TEXTURE0);
        GLES.glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTexture[0]);
        GLES.glActiveTexture(GL_TEXTURE1);
        GLES.glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTexture[1]);
        GLES.glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevReadFBO);
        GLES.glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDrawFBO);
        GLES.glGetIntegerv(GL_RENDERBUFFER_BINDING, &prevRenderbuffer);
        GLES.glGetIntegerv(GL_VIEWPORT, prevViewport);
    }

    ~GLStateGuard() {
        GLES.glUseProgram(prevProgram);
        GLES.glBindVertexArray(prevVAO);
        GLES.glBindBuffer(GL_ARRAY_BUFFER, prevArrayBuffer);
        GLES.glActiveTexture(GL_TEXTURE0);
        GLES.glBindTexture(GL_TEXTURE_2D, prevTexture[0]);
        GLES.glActiveTexture(GL_TEXTURE1);
        GLES.glBindTexture(GL_TEXTURE_2D, prevTexture[1]);
        GLES.glActiveTexture(prevActiveTexture);
        GLES.glBindRenderbuffer(GL_RENDERBUFFER, prevRenderbuffer);
        GLES.glBindFramebuffer(GL_READ_FRAMEBUFFER, prevReadFBO);
        GLES.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prevDrawFBO);
        GLES.glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    }
};

namespace FSR3FG_Context {
    GLuint g_prevFrameTex = 0;
    GLuint g_currFrameTex = 0;
    GLuint g_interpFrameTex = 0;

    GLuint g_prevLumaTex = 0;
    GLuint g_currLumaTex = 0;

    GLuint g_flowTex = 0;

    GLuint g_lumaFBO = 0;
    GLuint g_flowFBO = 0;
    GLuint g_interpFBO = 0;

    GLuint g_quadVAO = 0;
    GLuint g_quadVBO = 0;

    GLuint g_lumaProgram = 0;
    GLuint g_flowProgram = 0;
    GLuint g_interpProgram = 0;

    GLsizei g_width = 0;
    GLsizei g_height = 0;
    bool g_hasPrev = false;
}

static GLuint CompileProgram(const char* vsSource, const char* fsSource) {
    GLuint program = glCreateProgram();

    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &vsSource, nullptr);
    glCompileShader(vs);

    GLint status;
    glGetShaderiv(vs, GL_COMPILE_STATUS, &status);
    if (!status) {
        char log[512];
        glGetShaderInfoLog(vs, 512, nullptr, log);
        LOG_F("[FSR3 FG] Vertex shader error: %s\n", log);
        glDeleteShader(vs);
        glDeleteProgram(program);
        return 0;
    }

    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &fsSource, nullptr);
    glCompileShader(fs);

    glGetShaderiv(fs, GL_COMPILE_STATUS, &status);
    if (!status) {
        char log[512];
        glGetShaderInfoLog(fs, 512, nullptr, log);
        LOG_F("[FSR3 FG] Fragment shader error: %s\n", log);
        glDeleteShader(vs);
        glDeleteShader(fs);
        glDeleteProgram(program);
        return 0;
    }

    glBindAttribLocation(program, 0, "aPos");
    glBindAttribLocation(program, 1, "aTexCoord");

    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);

    glGetProgramiv(program, GL_LINK_STATUS, &status);
    if (!status) {
        char log[512];
        glGetProgramInfoLog(program, 512, nullptr, log);
        LOG_F("[FSR3 FG] Program link error: %s\n", log);
        glDeleteShader(vs);
        glDeleteShader(fs);
        glDeleteProgram(program);
        return 0;
    }

    glDeleteShader(vs);
    glDeleteShader(fs);

    return program;
}

static void InitQuad() {
    const float quadVertices[] = {-1.0f, 1.0f, 0.0f, 1.0f, -1.0f, -1.0f, 0.0f, 0.0f, 1.0f, -1.0f, 1.0f, 0.0f,
                                  -1.0f, 1.0f, 0.0f, 1.0f, 1.0f,  -1.0f, 1.0f, 0.0f, 1.0f, 1.0f,  1.0f, 1.0f};

    GLES.glGenVertexArrays(1, &FSR3FG_Context::g_quadVAO);
    GLES.glGenBuffers(1, &FSR3FG_Context::g_quadVBO);

    GLES.glBindVertexArray(FSR3FG_Context::g_quadVAO);
    GLES.glBindBuffer(GL_ARRAY_BUFFER, FSR3FG_Context::g_quadVBO);
    GLES.glBufferData(GL_ARRAY_BUFFER, sizeof(quadVertices), quadVertices, GL_STATIC_DRAW);

    GLES.glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    GLES.glEnableVertexAttribArray(0);
    GLES.glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    GLES.glEnableVertexAttribArray(1);

    GLES.glBindBuffer(GL_ARRAY_BUFFER, 0);
    GLES.glBindVertexArray(0);
}

static void BindQuad() {
    GLES.glBindVertexArray(FSR3FG_Context::g_quadVAO);
    GLES.glDrawArrays(GL_TRIANGLES, 0, 6);
    GLES.glBindVertexArray(0);
}

static GLuint CreateTextureRGBA8(GLsizei w, GLsizei h) {
    GLuint tex;
    GLES.glGenTextures(1, &tex);
    GLES.glBindTexture(GL_TEXTURE_2D, tex);
    GLES.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    return tex;
}

static GLuint CreateTextureLuma8(GLsizei w, GLsizei h) {
    GLuint tex;
    GLES.glGenTextures(1, &tex);
    GLES.glBindTexture(GL_TEXTURE_2D, tex);
    GLES.glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, w, h, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, nullptr);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    return tex;
}

static void DeleteTextures() {
    if (FSR3FG_Context::g_prevFrameTex) { GLES.glDeleteTextures(1, &FSR3FG_Context::g_prevFrameTex); FSR3FG_Context::g_prevFrameTex = 0; }
    if (FSR3FG_Context::g_currFrameTex) { GLES.glDeleteTextures(1, &FSR3FG_Context::g_currFrameTex); FSR3FG_Context::g_currFrameTex = 0; }
    if (FSR3FG_Context::g_interpFrameTex) { GLES.glDeleteTextures(1, &FSR3FG_Context::g_interpFrameTex); FSR3FG_Context::g_interpFrameTex = 0; }
    if (FSR3FG_Context::g_prevLumaTex) { GLES.glDeleteTextures(1, &FSR3FG_Context::g_prevLumaTex); FSR3FG_Context::g_prevLumaTex = 0; }
    if (FSR3FG_Context::g_currLumaTex) { GLES.glDeleteTextures(1, &FSR3FG_Context::g_currLumaTex); FSR3FG_Context::g_currLumaTex = 0; }
    if (FSR3FG_Context::g_flowTex) { GLES.glDeleteTextures(1, &FSR3FG_Context::g_flowTex); FSR3FG_Context::g_flowTex = 0; }
}

static void DeleteFBOs() {
    if (FSR3FG_Context::g_lumaFBO) { GLES.glDeleteFramebuffers(1, &FSR3FG_Context::g_lumaFBO); FSR3FG_Context::g_lumaFBO = 0; }
    if (FSR3FG_Context::g_flowFBO) { GLES.glDeleteFramebuffers(1, &FSR3FG_Context::g_flowFBO); FSR3FG_Context::g_flowFBO = 0; }
    if (FSR3FG_Context::g_interpFBO) { GLES.glDeleteFramebuffers(1, &FSR3FG_Context::g_interpFBO); FSR3FG_Context::g_interpFBO = 0; }
}

bool fsr3Initialized = false;
void InitFSR3FGResources() {
    if (FSR3FG_Context::g_width == 0 || FSR3FG_Context::g_height == 0) {
        FSR3FG_Context::g_width = 1920;
        FSR3FG_Context::g_height = 1080;
    }

    fsr3Initialized = true;
    GLStateGuard state;

    DeleteTextures();
    DeleteFBOs();

    FSR3FG_Context::g_lumaProgram = CompileProgram(FSR3FG_VSSource, FSR3FG_LumaFSSource);
    if (!FSR3FG_Context::g_lumaProgram) {
        LOG_F("[FSR3 FG] Failed to compile luma shader!");
        fsr3Initialized = false;
        return;
    }

    FSR3FG_Context::g_flowProgram = CompileProgram(FSR3FG_VSSource, FSR3FG_FlowFSSource);
    if (!FSR3FG_Context::g_flowProgram) {
        LOG_F("[FSR3 FG] Failed to compile flow shader!");
        fsr3Initialized = false;
        return;
    }

    FSR3FG_Context::g_interpProgram = CompileProgram(FSR3FG_VSSource, FSR3FG_InterpFSSource);
    if (!FSR3FG_Context::g_interpProgram) {
        LOG_F("[FSR3 FG] Failed to compile interpolation shader!");
        fsr3Initialized = false;
        return;
    }

    InitQuad();

    GLsizei w = FSR3FG_Context::g_width;
    GLsizei h = FSR3FG_Context::g_height;

    FSR3FG_Context::g_prevFrameTex = CreateTextureRGBA8(w, h);
    FSR3FG_Context::g_currFrameTex = CreateTextureRGBA8(w, h);
    FSR3FG_Context::g_interpFrameTex = CreateTextureRGBA8(w, h);

    FSR3FG_Context::g_prevLumaTex = CreateTextureLuma8(w, h);
    FSR3FG_Context::g_currLumaTex = CreateTextureLuma8(w, h);

    FSR3FG_Context::g_flowTex = CreateTextureRGBA8(w, h);

    GLES.glGenFramebuffers(1, &FSR3FG_Context::g_lumaFBO);
    GLES.glBindFramebuffer(GL_FRAMEBUFFER, FSR3FG_Context::g_lumaFBO);
    GLES.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                FSR3FG_Context::g_currLumaTex, 0);
    GLenum fbStatus = GLES.glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (fbStatus != GL_FRAMEBUFFER_COMPLETE) {
        LOG_F("[FSR3 FG] Luma FBO incomplete: 0x%x", fbStatus);
        fsr3Initialized = false;
        return;
    }

    GLES.glGenFramebuffers(1, &FSR3FG_Context::g_flowFBO);
    GLES.glBindFramebuffer(GL_FRAMEBUFFER, FSR3FG_Context::g_flowFBO);
    GLES.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                FSR3FG_Context::g_flowTex, 0);
    fbStatus = GLES.glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (fbStatus != GL_FRAMEBUFFER_COMPLETE) {
        LOG_F("[FSR3 FG] Flow FBO incomplete: 0x%x", fbStatus);
        fsr3Initialized = false;
        return;
    }

    GLES.glGenFramebuffers(1, &FSR3FG_Context::g_interpFBO);
    GLES.glBindFramebuffer(GL_FRAMEBUFFER, FSR3FG_Context::g_interpFBO);
    GLES.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                FSR3FG_Context::g_interpFrameTex, 0);
    fbStatus = GLES.glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (fbStatus != GL_FRAMEBUFFER_COMPLETE) {
        LOG_F("[FSR3 FG] Interp FBO incomplete: 0x%x", fbStatus);
        fsr3Initialized = false;
        return;
    }

    GLES.glBindFramebuffer(GL_FRAMEBUFFER, 0);

    FSR3FG_Context::g_hasPrev = false;

    LOG_V("[FSR3 FG] Frame Generation initialized (%dx%d)", w, h);
}

void ApplyFSR3FG() {
    if (!fsr3Initialized) {
        LOG_W("[FSR3 FG] Not initialized, skipping");
        return;
    }

    GLStateGuard state;

    GLint viewport[4];
    GLES.glGetIntegerv(GL_VIEWPORT, viewport);
    GLsizei fbWidth = viewport[2] > 0 ? viewport[2] : FSR3FG_Context::g_width;
    GLsizei fbHeight = viewport[3] > 0 ? viewport[3] : FSR3FG_Context::g_height;

    if (fbWidth != FSR3FG_Context::g_width || fbHeight != FSR3FG_Context::g_height) {
        FSR3FG_Context::g_width = fbWidth;
        FSR3FG_Context::g_height = fbHeight;
        LOG_V("[FSR3 FG] Dimensions changed to %dx%d, reinitializing", fbWidth, fbHeight);
        fsr3Initialized = false;
        InitFSR3FGResources();
        if (!fsr3Initialized) return;
    }

    GLsizei w = FSR3FG_Context::g_width;
    GLsizei h = FSR3FG_Context::g_height;

    // Step 1: Capture current frame from backbuffer
    GLES.glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    GLES.glBindTexture(GL_TEXTURE_2D, FSR3FG_Context::g_currFrameTex);
    GLES.glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 0, 0, w, h, 0);

    // Step 2: Extract luminance from current frame
    GLES.glBindFramebuffer(GL_FRAMEBUFFER, FSR3FG_Context::g_lumaFBO);
    GLES.glViewport(0, 0, w, h);
    GLES.glClear(GL_COLOR_BUFFER_BIT);
    GLES.glUseProgram(FSR3FG_Context::g_lumaProgram);
    GLES.glActiveTexture(GL_TEXTURE0);
    GLES.glBindTexture(GL_TEXTURE_2D, FSR3FG_Context::g_currFrameTex);
    GLES.glUniform1i(glGetUniformLocation(FSR3FG_Context::g_lumaProgram, "uInputTex"), 0);
    BindQuad();

    if (FSR3FG_Context::g_hasPrev) {
        // Step 3: Estimate optical flow between previous and current luma
        GLES.glBindFramebuffer(GL_FRAMEBUFFER, FSR3FG_Context::g_flowFBO);
        GLES.glViewport(0, 0, w, h);
        GLES.glClear(GL_COLOR_BUFFER_BIT);
        GLES.glUseProgram(FSR3FG_Context::g_flowProgram);
        GLES.glActiveTexture(GL_TEXTURE0);
        GLES.glBindTexture(GL_TEXTURE_2D, FSR3FG_Context::g_prevLumaTex);
        GLES.glUniform1i(glGetUniformLocation(FSR3FG_Context::g_flowProgram, "uPrevLuma"), 0);
        GLES.glActiveTexture(GL_TEXTURE1);
        GLES.glBindTexture(GL_TEXTURE_2D, FSR3FG_Context::g_currLumaTex);
        GLES.glUniform1i(glGetUniformLocation(FSR3FG_Context::g_flowProgram, "uCurrLuma"), 1);
        glm::vec2 texelSize = {1.0f / w, 1.0f / h};
        GLES.glUniform2fv(glGetUniformLocation(FSR3FG_Context::g_flowProgram, "uTexelSize"), 1,
                          reinterpret_cast<const GLfloat*>(&texelSize));
        BindQuad();

        // Step 4: Generate interpolated frame using optical flow
        GLES.glBindFramebuffer(GL_FRAMEBUFFER, FSR3FG_Context::g_interpFBO);
        GLES.glViewport(0, 0, w, h);
        GLES.glClear(GL_COLOR_BUFFER_BIT);
        GLES.glUseProgram(FSR3FG_Context::g_interpProgram);
        GLES.glActiveTexture(GL_TEXTURE0);
        GLES.glBindTexture(GL_TEXTURE_2D, FSR3FG_Context::g_prevFrameTex);
        GLES.glUniform1i(glGetUniformLocation(FSR3FG_Context::g_interpProgram, "uPrevFrame"), 0);
        GLES.glActiveTexture(GL_TEXTURE1);
        GLES.glBindTexture(GL_TEXTURE_2D, FSR3FG_Context::g_currFrameTex);
        GLES.glUniform1i(glGetUniformLocation(FSR3FG_Context::g_interpProgram, "uCurrFrame"), 1);
        GLES.glActiveTexture(GL_TEXTURE2);
        GLES.glBindTexture(GL_TEXTURE_2D, FSR3FG_Context::g_flowTex);
        GLES.glUniform1i(glGetUniformLocation(FSR3FG_Context::g_interpProgram, "uFlowTex"), 2);
        GLES.glUniform2fv(glGetUniformLocation(FSR3FG_Context::g_interpProgram, "uTexelSize"), 1,
                          reinterpret_cast<const GLfloat*>(&texelSize));
        BindQuad();

        // Step 5: Blit interpolated frame to default framebuffer
        GLES.glBindFramebuffer(GL_READ_FRAMEBUFFER, FSR3FG_Context::g_interpFBO);
        GLES.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        GLES.glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_LINEAR);

        LOG_D("[FSR3 FG] Interpolated frame generated and applied");
    } else {
        LOG_D("[FSR3 FG] First frame captured, no interpolation yet");
    }

    // Rotate frame buffers: curr -> prev, currLuma -> prevLuma
    std::swap(FSR3FG_Context::g_prevFrameTex, FSR3FG_Context::g_currFrameTex);
    std::swap(FSR3FG_Context::g_prevLumaTex, FSR3FG_Context::g_currLumaTex);
    FSR3FG_Context::g_hasPrev = true;
}
