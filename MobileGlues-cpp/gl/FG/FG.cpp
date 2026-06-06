#include "FG.h"
#include "../../config/settings.h"

#define DEBUG 0

// GLSL shader source for frame interpolation
static const char* FG_VSSource = R"(
#version 100
attribute vec2 aPos;
attribute vec2 aTexCoord;
varying vec2 vTexCoord;
void main() {
    gl_Position = vec4(aPos.x, aPos.y, 0.0, 1.0);
    vTexCoord = aTexCoord;
}
)";

static const char* FG_FSSource = R"(
#version 100
precision mediump float;
varying vec2 vTexCoord;
uniform sampler2D uPrevTex;
uniform sampler2D uCurrTex;
uniform vec2 uTexelSize;

void main() {
    vec2 uv = vTexCoord;
    vec4 curr = texture2D(uCurrTex, uv);

    ivec2 searchRange = ivec2(5, 5);
    vec4 bestMatch = texture2D(uPrevTex, uv);
    float bestDiff = distance(bestMatch, curr);
    vec2 bestOffset = vec2(0.0, 0.0);

    for (int dy = -searchRange.y; dy <= searchRange.y; dy++) {
        for (int dx = -searchRange.x; dx <= searchRange.x; dx++) {
            vec2 offset = vec2(float(dx), float(dy)) * uTexelSize;
            vec2 sampleUV = uv + offset;
            if (sampleUV.x < 0.0 || sampleUV.x > 1.0 ||
                sampleUV.y < 0.0 || sampleUV.y > 1.0) continue;
            vec4 sample = texture2D(uPrevTex, sampleUV);
            float diff = distance(sample, curr);
            if (diff < bestDiff) {
                bestDiff = diff;
                bestMatch = sample;
                bestOffset = offset;
            }
        }
    }

    vec2 forwardUV = uv + bestOffset;
    vec4 forward = texture2D(uPrevTex, forwardUV);

    // Confidence-based: if motion match is poor, use current frame to avoid ghosting
    float confidence = 1.0 - bestDiff * 1.5;
    confidence = clamp(confidence, 0.0, 1.0);

    vec4 result = mix(curr, forward, confidence * 0.5);
    gl_FragColor = result;
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

namespace FG_Context {
    GLuint g_prevFrameTex = 0;
    GLuint g_currFrameTex = 0;
    GLuint g_interpFrameTex = 0;
    GLuint g_fgFBO = 0;
    GLuint g_quadVAO = 0;
    GLuint g_quadVBO = 0;
    GLuint g_interpProgram = 0;

    GLsizei g_width = 0;
    GLsizei g_height = 0;
    bool g_hasPrev = false;
    bool g_doubledThisFrame = false;
}

GLuint CompileFGShader() {
    GLuint program = glCreateProgram();

    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &FG_VSSource, nullptr);
    glCompileShader(vs);

    GLint status;
    glGetShaderiv(vs, GL_COMPILE_STATUS, &status);
    if (!status) {
        char log[512];
        glGetShaderInfoLog(vs, 512, nullptr, log);
        LOG_F("FG vertex shader error: %s\n", log);
        return 0;
    }

    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &FG_FSSource, nullptr);
    glCompileShader(fs);

    glGetShaderiv(fs, GL_COMPILE_STATUS, &status);
    if (!status) {
        char log[512];
        glGetShaderInfoLog(fs, 512, nullptr, log);
        LOG_F("FG fragment shader error: %s\n", log);
        return 0;
    }

    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);

    glGetProgramiv(program, GL_LINK_STATUS, &status);
    if (!status) {
        char log[512];
        glGetProgramInfoLog(program, 512, nullptr, log);
        LOG_F("FG program link error: %s\n", log);
        return 0;
    }

    glDeleteShader(vs);
    glDeleteShader(fs);

    return program;
}

void InitFGQuad() {
    const float quadVertices[] = {-1.0f, 1.0f, 0.0f, 1.0f, -1.0f, -1.0f, 0.0f, 0.0f, 1.0f, -1.0f, 1.0f, 0.0f,
                                  -1.0f, 1.0f, 0.0f, 1.0f, 1.0f,  -1.0f, 1.0f, 0.0f, 1.0f, 1.0f,  1.0f, 1.0f};

    GLES.glGenVertexArrays(1, &FG_Context::g_quadVAO);
    GLES.glGenBuffers(1, &FG_Context::g_quadVBO);

    GLES.glBindVertexArray(FG_Context::g_quadVAO);
    GLES.glBindBuffer(GL_ARRAY_BUFFER, FG_Context::g_quadVBO);
    GLES.glBufferData(GL_ARRAY_BUFFER, sizeof(quadVertices), quadVertices, GL_STATIC_DRAW);

    GLES.glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    GLES.glEnableVertexAttribArray(0);
    GLES.glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    GLES.glEnableVertexAttribArray(1);

    GLES.glBindBuffer(GL_ARRAY_BUFFER, 0);
    GLES.glBindVertexArray(0);
}

bool fgInitialized = false;
void InitFGResources() {
    if (FG_Context::g_width == 0 || FG_Context::g_height == 0) {
        FG_Context::g_width = 1920;
        FG_Context::g_height = 1080;
    }

    fgInitialized = true;
    GLStateGuard state;

    FG_Context::g_interpProgram = CompileFGShader();
    if (!FG_Context::g_interpProgram) {
        LOG_F("Failed to compile FG shader program!");
        fgInitialized = false;
        return;
    }

    glUseProgram(FG_Context::g_interpProgram);
    glUniform1i(glGetUniformLocation(FG_Context::g_interpProgram, "uPrevTex"), 0);
    glUniform1i(glGetUniformLocation(FG_Context::g_interpProgram, "uCurrTex"), 1);
    glUseProgram(0);

    InitFGQuad();

    GLES.glGenTextures(1, &FG_Context::g_prevFrameTex);
    GLES.glBindTexture(GL_TEXTURE_2D, FG_Context::g_prevFrameTex);
    GLES.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, FG_Context::g_width, FG_Context::g_height, 0, GL_RGBA,
                      GL_UNSIGNED_BYTE, nullptr);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);

    GLES.glGenTextures(1, &FG_Context::g_currFrameTex);
    GLES.glBindTexture(GL_TEXTURE_2D, FG_Context::g_currFrameTex);
    GLES.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, FG_Context::g_width, FG_Context::g_height, 0, GL_RGBA,
                      GL_UNSIGNED_BYTE, nullptr);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);

    GLES.glGenTextures(1, &FG_Context::g_interpFrameTex);
    GLES.glBindTexture(GL_TEXTURE_2D, FG_Context::g_interpFrameTex);
    GLES.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, FG_Context::g_width, FG_Context::g_height, 0, GL_RGBA,
                      GL_UNSIGNED_BYTE, nullptr);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);

    GLES.glGenFramebuffers(1, &FG_Context::g_fgFBO);
    GLES.glBindFramebuffer(GL_FRAMEBUFFER, FG_Context::g_fgFBO);
    GLES.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, FG_Context::g_interpFrameTex, 0);

    GLenum fbStatus = GLES.glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (fbStatus != GL_FRAMEBUFFER_COMPLETE) {
        LOG_F("FG framebuffer incomplete: 0x%x", fbStatus);
        fgInitialized = false;
        return;
    }

    GLES.glBindFramebuffer(GL_FRAMEBUFFER, 0);

    FG_Context::g_hasPrev = false;
    FG_Context::g_doubledThisFrame = false;

    LOG_V("[FG] Frame Generation initialized (%dx%d)", FG_Context::g_width, FG_Context::g_height);
}

void CaptureFrame() {
    GLStateGuard state;

    GLint readFBO = 0;
    GLES.glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFBO);

    GLES.glBindFramebuffer(GL_FRAMEBUFFER, 0);

    GLint viewport[4];
    GLES.glGetIntegerv(GL_VIEWPORT, viewport);
    GLsizei fbWidth = viewport[2];
    GLsizei fbHeight = viewport[3];

    if (fbWidth <= 0 || fbHeight <= 0) return;

    GLsizei capWidth = (fbWidth + 1) & ~1;
    GLsizei capHeight = (fbHeight + 1) & ~1;

    GLES.glGenTextures(1, &FG_Context::g_currFrameTex);
    GLES.glBindTexture(GL_TEXTURE_2D, FG_Context::g_currFrameTex);
    GLES.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, capWidth, capHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);

    GLES.glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 0, 0, capWidth, capHeight, 0);

    GLES.glBindFramebuffer(GL_READ_FRAMEBUFFER, readFBO);

    LOG_D("[FG] Frame captured: %dx%d", capWidth, capHeight);
}

void ApplyFG() {
    if (!fgInitialized || !FG_Context::g_interpProgram) {
        LOG_W("FG not initialized, skipping");
        return;
    }

    GLStateGuard state;

    // Capture current backbuffer
    GLint viewport[4];
    GLES.glGetIntegerv(GL_VIEWPORT, viewport);
    GLsizei fbWidth = viewport[2] > 0 ? viewport[2] : FG_Context::g_width;
    GLsizei fbHeight = viewport[3] > 0 ? viewport[3] : FG_Context::g_height;

    // Reinitialize if dimensions changed
    if (fbWidth != FG_Context::g_width || fbHeight != FG_Context::g_height) {
        FG_Context::g_width = fbWidth;
        FG_Context::g_height = fbHeight;
        LOG_V("[FG] Dimensions changed to %dx%d, reinitializing", fbWidth, fbHeight);
        fgInitialized = false;
        InitFGResources();
        if (!fgInitialized) return;
    }

    // Capture current frame from default FBO
    GLES.glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    GLES.glBindTexture(GL_TEXTURE_2D, FG_Context::g_currFrameTex);
    GLES.glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 0, 0, FG_Context::g_width, FG_Context::g_height, 0);

    if (FG_Context::g_hasPrev) {
        LOG_D("[FG] Generating interpolated frame");

        // Render interpolated frame to FBO
        GLES.glBindFramebuffer(GL_FRAMEBUFFER, FG_Context::g_fgFBO);
        GLES.glViewport(0, 0, FG_Context::g_width, FG_Context::g_height);

        GLES.glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        GLES.glClear(GL_COLOR_BUFFER_BIT);

        GLES.glUseProgram(FG_Context::g_interpProgram);

        GLES.glActiveTexture(GL_TEXTURE0);
        GLES.glBindTexture(GL_TEXTURE_2D, FG_Context::g_prevFrameTex);

        GLES.glActiveTexture(GL_TEXTURE1);
        GLES.glBindTexture(GL_TEXTURE_2D, FG_Context::g_currFrameTex);

        glm::vec2 texelSize = {1.0f / FG_Context::g_width, 1.0f / FG_Context::g_height};
        GLES.glUniform2fv(glGetUniformLocation(FG_Context::g_interpProgram, "uTexelSize"), 1,
                          reinterpret_cast<const GLfloat*>(&texelSize));

        GLES.glBindVertexArray(FG_Context::g_quadVAO);
        GLES.glDrawArrays(GL_TRIANGLES, 0, 6);
        GLES.glBindVertexArray(0);

        // Blit interpolated frame to default framebuffer
        GLES.glBindFramebuffer(GL_READ_FRAMEBUFFER, FG_Context::g_fgFBO);
        GLES.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        GLES.glBlitFramebuffer(0, 0, FG_Context::g_width, FG_Context::g_height, 0, 0, FG_Context::g_width,
                               FG_Context::g_height, GL_COLOR_BUFFER_BIT, GL_LINEAR);

        GLES.glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

        LOG_D("[FG] Interpolated frame generated and applied");
    } else {
        LOG_D("[FG] First frame captured, no interpolation yet");
    }

    // Rotate frame buffers: curr -> prev
    std::swap(FG_Context::g_prevFrameTex, FG_Context::g_currFrameTex);
    FG_Context::g_hasPrev = true;
}
