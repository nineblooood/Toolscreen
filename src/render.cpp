#include "render.h"
#include "fake_cursor.h"
#include "gui.h"
#include "logic_thread.h"
#include "mirror_thread.h"
#include "obs_thread.h"
#include "profiler.h"
#include "render_thread.h"
#include "stb_image.h"
#include "utils.h"
#include "window_overlay.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <shared_mutex>
#include <unordered_map>

static std::unordered_map<std::string, size_t> s_mirrorLookupCache;
static std::unordered_map<std::string, size_t> s_imageLookupCache;
static std::unordered_map<std::string, size_t> s_windowOverlayLookupCache;
static std::atomic<uint64_t> s_configCacheVersion{ 0 };
static uint64_t s_lastCacheRebuildVersion = 0;
static std::mutex s_lookupCacheMutex;

static void RebuildConfigLookupCaches() {
    s_mirrorLookupCache.clear();
    s_imageLookupCache.clear();
    s_windowOverlayLookupCache.clear();

    s_mirrorLookupCache.reserve(g_config.mirrors.size());
    for (size_t i = 0; i < g_config.mirrors.size(); ++i) { s_mirrorLookupCache[g_config.mirrors[i].name] = i; }

    s_imageLookupCache.reserve(g_config.images.size());
    for (size_t i = 0; i < g_config.images.size(); ++i) { s_imageLookupCache[g_config.images[i].name] = i; }

    s_windowOverlayLookupCache.reserve(g_config.windowOverlays.size());
    for (size_t i = 0; i < g_config.windowOverlays.size(); ++i) { s_windowOverlayLookupCache[g_config.windowOverlays[i].name] = i; }
}

void InvalidateConfigLookupCaches() { s_configCacheVersion.fetch_add(1, std::memory_order_release); }

static void EnsureConfigCachesValid() {
    uint64_t currentVersion = s_configCacheVersion.load(std::memory_order_acquire);
    if (g_configIsDirty) {
        InvalidateConfigLookupCaches();
        currentVersion = s_configCacheVersion.load(std::memory_order_acquire);
    }
    if (currentVersion != s_lastCacheRebuildVersion) {
        std::lock_guard<std::mutex> lock(s_lookupCacheMutex);
        // Double-check after acquiring lock
        currentVersion = s_configCacheVersion.load(std::memory_order_acquire);
        if (currentVersion != s_lastCacheRebuildVersion) {
            RebuildConfigLookupCaches();
            s_lastCacheRebuildVersion = currentVersion;
        }
    }
}

extern std::atomic<bool> g_graphicsHookDetected;

GLuint g_filterProgram = 0;
GLuint g_renderProgram = 0;
GLuint g_backgroundProgram = 0;
GLuint g_solidColorProgram = 0;
GLuint g_imageRenderProgram = 0;
GLuint g_passthroughProgram = 0;
GLuint g_gradientProgram = 0;

FilterShaderLocs g_filterShaderLocs;
RenderShaderLocs g_renderShaderLocs;
BackgroundShaderLocs g_backgroundShaderLocs;
SolidColorShaderLocs g_solidColorShaderLocs;
ImageRenderShaderLocs g_imageRenderShaderLocs;
PassthroughShaderLocs g_passthroughShaderLocs;
GradientShaderLocs g_gradientShaderLocs;

std::atomic<bool> g_shouldRenderGui{ false };
std::atomic<bool> g_showPerformanceOverlay{ false };
std::atomic<bool> g_showProfiler{ false };
std::atomic<bool> g_showEyeZoom{ false };
std::atomic<float> g_eyeZoomFadeOpacity{ 1.0f };
std::atomic<int> g_eyeZoomAnimatedViewportX{ -1 };
std::atomic<bool> g_isTransitioningFromEyeZoom{ false };
std::atomic<bool> g_showTextureGrid{ false };
std::atomic<int> g_textureGridModeWidth{ 0 };
std::atomic<int> g_textureGridModeHeight{ 0 };

static GLuint s_eyeZoomSnapshotTexture = 0;
static GLuint s_eyeZoomSnapshotFBO = 0;
static int s_eyeZoomSnapshotWidth = 0;
static int s_eyeZoomSnapshotHeight = 0;
static bool s_eyeZoomSnapshotValid = false;

static GLuint s_eyeZoomTempFBO = 0;
static GLuint s_eyeZoomTempTexture = 0;
static int s_eyeZoomTempWidth = 0;
static int s_eyeZoomTempHeight = 0;
static GLuint s_eyeZoomBlitFBO = 0;

GLuint GetEyeZoomSnapshotTexture() { return s_eyeZoomSnapshotTexture; }
int GetEyeZoomSnapshotWidth() { return s_eyeZoomSnapshotWidth; }
int GetEyeZoomSnapshotHeight() { return s_eyeZoomSnapshotHeight; }

std::unordered_map<std::string, MirrorInstance> g_mirrorInstances;
std::unordered_map<std::string, BackgroundTextureInstance> g_backgroundTextures;
std::unordered_map<std::string, UserImageInstance> g_userImages;
GLuint g_vao = 0;
GLuint g_vbo = 0;
GLuint g_debugVAO = 0;
GLuint g_debugVBO = 0;
GLuint g_sceneFBO = 0;
GLuint g_sceneTexture = 0;
int g_sceneW = 0;
int g_sceneH = 0;

GLuint g_fullscreenQuadVAO = 0;
GLuint g_fullscreenQuadVBO = 0;

// These maps are accessed from multiple threads (render + GUI)
std::shared_mutex g_mirrorInstancesMutex;
std::mutex g_userImagesMutex;
std::mutex g_backgroundTexturesMutex;

std::vector<GLuint> g_texturesToDelete;
std::mutex g_texturesToDeleteMutex;
std::atomic<bool> g_hasTexturesToDelete{ false };
std::atomic<bool> g_glInitialized{ false };
std::atomic<bool> g_isGameFocused{ true };
GameViewportGeometry g_lastFrameGeometry;
std::mutex g_geometryMutex;

// Fence for async overlay blit - created after blit, waited on before SwapBuffers if setting enabled
static std::atomic<GLsync> g_overlayBlitFence{ nullptr };

std::string s_hoveredImageName = "";
std::string s_draggedImageName = "";
bool s_isDragging = false;
POINT s_lastMousePos = { 0, 0 };
POINT s_dragStartPos = { 0, 0 };

std::string s_hoveredWindowOverlayName = "";
std::string s_draggedWindowOverlayName = "";
bool s_isWindowOverlayDragging = false;
POINT s_windowOverlayDragStart = { 0, 0 };
int s_initialX = 0, s_initialY = 0;

struct EyeZoomTextLabel {
    int number;
    float centerX;
    float centerY;
    Color color;
};
static std::vector<EyeZoomTextLabel> s_eyezoomTextLabels;
static std::mutex s_eyezoomTextMutex;

struct TextureGridLabel {
    GLuint textureId;
    float x;
    float y;
    int tileSize;
    int width;
    int height;
    float sizeMB;
    GLenum internalFormat;
    GLint minFilter;
    GLint magFilter;
    GLint wrapS;
    GLint wrapT;
};
static std::vector<TextureGridLabel> s_textureGridLabels;
static std::mutex s_textureGridMutex;

static ImFont* g_overlayTextFont = nullptr;
static float g_overlayTextFontSize = 24.0f;

static void CacheEyeZoomTextLabel(int number, float centerX, float centerY, const Color& color) {
    std::lock_guard<std::mutex> lock(s_eyezoomTextMutex);
    s_eyezoomTextLabels.push_back({ number, centerX, centerY, color });
}

void DrawOverlayBorder(float nx1, float ny1, float nx2, float ny2, float borderWidth, float borderHeight, bool isDragging,
                       bool drawCorners = false) {
    glUseProgram(g_solidColorProgram);
    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    if (isDragging) {
        glUniform4f(g_solidColorShaderLocs.color, 0.0f, 1.0f, 0.0f, 0.8f);
    } else {
        glUniform4f(g_solidColorShaderLocs.color, 1.0f, 1.0f, 0.0f, 0.6f);
    }

    float allBorders[24 * 4] = {
                                 nx1, ny2 - borderHeight, 0, 0, nx2, ny2 - borderHeight, 0, 0, nx2, ny2, 0, 0, nx1, ny2 - borderHeight, 0,
                                 0, nx2, ny2, 0, 0, nx1, ny2, 0, 0,

                                 nx1, ny1, 0, 0, nx2, ny1, 0, 0, nx2, ny1 + borderHeight, 0, 0, nx1, ny1, 0, 0, nx2, ny1 + borderHeight, 0,
                                 0, nx1, ny1 + borderHeight, 0, 0,

                                 nx1, ny1, 0, 0, nx1 + borderWidth, ny1, 0, 0, nx1 + borderWidth, ny2, 0, 0, nx1, ny1, 0, 0,
                                 nx1 + borderWidth, ny2, 0, 0, nx1, ny2, 0, 0,

                                 nx2 - borderWidth, ny1, 0, 0, nx2, ny1, 0, 0, nx2, ny2, 0, 0, nx2 - borderWidth, ny1, 0, 0, nx2, ny2, 0, 0,
                                 nx2 - borderWidth, ny2, 0, 0
    };

    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(allBorders), allBorders);
    glDrawArrays(GL_TRIANGLES, 0, 24);

    if (drawCorners) {
        float cornerSize = borderWidth * 2.5f;
        float cornerSizeH = borderHeight * 2.5f;

        glUniform4f(g_solidColorShaderLocs.color, 1.0f, 0.5f, 0.0f, 0.9f);

        float allCorners[24 * 4] = {
                                     nx1, ny2 - cornerSizeH, 0, 0, nx1 + cornerSize, ny2 - cornerSizeH, 0, 0, nx1 + cornerSize, ny2, 0, 0,
                                     nx1, ny2 - cornerSizeH, 0, 0, nx1 + cornerSize, ny2, 0, 0, nx1, ny2, 0, 0,

                                     nx2 - cornerSize, ny2 - cornerSizeH, 0, 0, nx2, ny2 - cornerSizeH, 0, 0, nx2, ny2, 0, 0,
                                     nx2 - cornerSize, ny2 - cornerSizeH, 0, 0, nx2, ny2, 0, 0, nx2 - cornerSize, ny2, 0, 0,

                                     nx1, ny1, 0, 0, nx1 + cornerSize, ny1, 0, 0, nx1 + cornerSize, ny1 + cornerSizeH, 0, 0, nx1, ny1, 0, 0,
                                     nx1 + cornerSize, ny1 + cornerSizeH, 0, 0, nx1, ny1 + cornerSizeH, 0, 0,

                                     nx2 - cornerSize, ny1, 0, 0, nx2, ny1, 0, 0, nx2, ny1 + cornerSizeH, 0, 0, nx2 - cornerSize, ny1, 0, 0,
                                     nx2, ny1 + cornerSizeH, 0, 0, nx2 - cornerSize, ny1 + cornerSizeH, 0, 0
        };

        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(allCorners), allCorners);
        glDrawArrays(GL_TRIANGLES, 0, 24);
    }

    glDisable(GL_BLEND);
}

void RenderGameBorder(int x, int y, int w, int h, int borderWidth, int radius, const Color& color, int fullW, int fullH) {
    if (borderWidth <= 0) return;

    glUseProgram(g_solidColorProgram);
    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glUniform4f(g_solidColorShaderLocs.color, color.r, color.g, color.b, 1.0f);

    int y_gl = fullH - y - h;

    int outerLeft = x - borderWidth;
    int outerRight = x + w + borderWidth;
    int outerBottom = y_gl - borderWidth;
    int outerTop = y_gl + h + borderWidth;

    int effectiveRadius = radius;
    int maxRadius = (w < h ? w : h) / 2 + borderWidth;
    if (effectiveRadius > maxRadius) effectiveRadius = maxRadius;

    auto toNdcX = [fullW](int px) { return (static_cast<float>(px) / fullW) * 2.0f - 1.0f; };
    auto toNdcY = [fullH](int py) { return (static_cast<float>(py) / fullH) * 2.0f - 1.0f; };

    if (effectiveRadius <= 0) {
        float allBorders[] = {
            toNdcX(outerLeft),  toNdcY(y_gl + h), 0, 0, toNdcX(outerRight), toNdcY(y_gl + h), 0, 0,
            toNdcX(outerRight), toNdcY(outerTop),  0, 0, toNdcX(outerLeft),  toNdcY(y_gl + h), 0, 0,
            toNdcX(outerRight), toNdcY(outerTop),  0, 0, toNdcX(outerLeft),  toNdcY(outerTop),  0, 0,
            toNdcX(outerLeft),  toNdcY(outerBottom), 0, 0, toNdcX(outerRight), toNdcY(outerBottom), 0, 0,
            toNdcX(outerRight), toNdcY(y_gl),        0, 0, toNdcX(outerLeft),  toNdcY(outerBottom), 0, 0,
            toNdcX(outerRight), toNdcY(y_gl),        0, 0, toNdcX(outerLeft),  toNdcY(y_gl),        0, 0,
            toNdcX(outerLeft), toNdcY(y_gl),     0, 0, toNdcX(x),         toNdcY(y_gl),     0, 0,
            toNdcX(x),         toNdcY(y_gl + h), 0, 0, toNdcX(outerLeft), toNdcY(y_gl),     0, 0,
            toNdcX(x),         toNdcY(y_gl + h), 0, 0, toNdcX(outerLeft), toNdcY(y_gl + h), 0, 0,
            toNdcX(x + w),      toNdcY(y_gl),     0, 0, toNdcX(outerRight), toNdcY(y_gl),     0, 0,
            toNdcX(outerRight), toNdcY(y_gl + h), 0, 0, toNdcX(x + w),      toNdcY(y_gl),     0, 0,
            toNdcX(outerRight), toNdcY(y_gl + h), 0, 0, toNdcX(x + w),      toNdcY(y_gl + h), 0, 0
        };
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(allBorders), allBorders);
        glDrawArrays(GL_TRIANGLES, 0, 24);
    } else {
        int segments = 8;

        float straightBorders[] = {
            toNdcX(x + effectiveRadius),     toNdcY(y_gl + h), 0, 0, toNdcX(x + w - effectiveRadius), toNdcY(y_gl + h), 0, 0,
            toNdcX(x + w - effectiveRadius), toNdcY(outerTop), 0, 0, toNdcX(x + effectiveRadius),     toNdcY(y_gl + h), 0, 0,
            toNdcX(x + w - effectiveRadius), toNdcY(outerTop), 0, 0, toNdcX(x + effectiveRadius),     toNdcY(outerTop), 0, 0,
            toNdcX(x + effectiveRadius),     toNdcY(outerBottom), 0, 0, toNdcX(x + w - effectiveRadius), toNdcY(outerBottom), 0, 0,
            toNdcX(x + w - effectiveRadius), toNdcY(y_gl),        0, 0, toNdcX(x + effectiveRadius),     toNdcY(outerBottom), 0, 0,
            toNdcX(x + w - effectiveRadius), toNdcY(y_gl),        0, 0, toNdcX(x + effectiveRadius),     toNdcY(y_gl),        0, 0,
            toNdcX(outerLeft), toNdcY(y_gl + effectiveRadius),     0, 0, toNdcX(x),         toNdcY(y_gl + effectiveRadius),     0, 0,
            toNdcX(x),         toNdcY(y_gl + h - effectiveRadius), 0, 0, toNdcX(outerLeft), toNdcY(y_gl + effectiveRadius),     0, 0,
            toNdcX(x),         toNdcY(y_gl + h - effectiveRadius), 0, 0, toNdcX(outerLeft), toNdcY(y_gl + h - effectiveRadius), 0, 0,
            toNdcX(x + w),      toNdcY(y_gl + effectiveRadius),     0, 0, toNdcX(outerRight), toNdcY(y_gl + effectiveRadius),     0, 0,
            toNdcX(outerRight), toNdcY(y_gl + h - effectiveRadius), 0, 0, toNdcX(x + w),      toNdcY(y_gl + effectiveRadius),     0, 0,
            toNdcX(outerRight), toNdcY(y_gl + h - effectiveRadius), 0, 0, toNdcX(x + w),      toNdcY(y_gl + h - effectiveRadius), 0, 0
        };
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(straightBorders), straightBorders);
        glDrawArrays(GL_TRIANGLES, 0, 24);

        auto renderCornerArc = [&](float centerX, float centerY, float innerR, float outerR, float startAngle, float endAngle) {
            float angleStep = (endAngle - startAngle) / segments;
            std::vector<float> arcVerts;
            arcVerts.reserve(segments * 6 * 4);
            for (int s = 0; s < segments; s++) {
                float a1 = startAngle + s * angleStep;
                float a2 = startAngle + (s + 1) * angleStep;

                float c1 = cosf(a1), s1 = sinf(a1);
                float c2 = cosf(a2), s2 = sinf(a2);

                float tri[] = {
                    toNdcX((int)(centerX + innerR * c1)), toNdcY((int)(centerY + innerR * s1)), 0, 0,
                    toNdcX((int)(centerX + outerR * c1)), toNdcY((int)(centerY + outerR * s1)), 0, 0,
                    toNdcX((int)(centerX + outerR * c2)), toNdcY((int)(centerY + outerR * s2)), 0, 0,
                    toNdcX((int)(centerX + innerR * c1)), toNdcY((int)(centerY + innerR * s1)), 0, 0,
                    toNdcX((int)(centerX + outerR * c2)), toNdcY((int)(centerY + outerR * s2)), 0, 0,
                    toNdcX((int)(centerX + innerR * c2)), toNdcY((int)(centerY + innerR * s2)), 0, 0
                };
                arcVerts.insert(arcVerts.end(), std::begin(tri), std::end(tri));
            }
            glBufferSubData(GL_ARRAY_BUFFER, 0, arcVerts.size() * sizeof(float), arcVerts.data());
            glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(arcVerts.size() / 4));
        };

        const float PI = 3.14159265358979323846f;
        float innerR = (float)effectiveRadius;
        float outerR = (float)(effectiveRadius + borderWidth);

        renderCornerArc((float)(x + effectiveRadius), (float)(y_gl + h - effectiveRadius), innerR, outerR, PI * 0.5f, PI);

        renderCornerArc((float)(x + w - effectiveRadius), (float)(y_gl + h - effectiveRadius), innerR, outerR, 0.0f, PI * 0.5f);

        renderCornerArc((float)(x + effectiveRadius), (float)(y_gl + effectiveRadius), innerR, outerR, PI, PI * 1.5f);

        renderCornerArc((float)(x + w - effectiveRadius), (float)(y_gl + effectiveRadius), innerR, outerR, PI * 1.5f, PI * 2.0f);
    }

    glDisable(GL_BLEND);
}

void CalculateImageDimensions(const ImageConfig& img, int& outW, int& outH) {
    // NOTE: This is used in UI/drag hit-testing; avoid blocking if another thread is updating textures.
    std::unique_lock<std::mutex> lock(g_userImagesMutex, std::try_to_lock);
    if (lock.owns_lock()) {
        auto it = g_userImages.find(img.name);
        if (it != g_userImages.end() && it->second.textureId != 0) {
            int texWidth = it->second.width;
            int texHeight = it->second.height;
            int croppedWidth = texWidth - img.crop_left - img.crop_right;
            int croppedHeight = texHeight - img.crop_top - img.crop_bottom;
            if (croppedWidth < 1) croppedWidth = 1;
            if (croppedHeight < 1) croppedHeight = 1;
            outW = static_cast<int>(croppedWidth * img.scale);
            outH = static_cast<int>(croppedHeight * img.scale);
            if (outW < 1) outW = 1;
            if (outH < 1) outH = 1;
            return;
        }
    }

    // Default size if texture not loaded / mutex busy
    outW = static_cast<int>(100 * img.scale);
    outH = static_cast<int>(100 * img.scale);
    if (outW < 1) outW = 1;
    if (outH < 1) outH = 1;
}

// Helper to calculate dimensions when mutex is already held (faster path)
static void CalculateWindowOverlayDimensionsUnsafe(const WindowOverlayConfig& overlay, int& outW, int& outH) {
    // NOTE: Caller must hold g_windowOverlayCacheMutex
    auto it = g_windowOverlayCache.find(overlay.name);
    if (it != g_windowOverlayCache.end() && it->second) {
        int texWidth = it->second->glTextureWidth;
        int texHeight = it->second->glTextureHeight;
        int croppedWidth = texWidth - overlay.crop_left - overlay.crop_right;
        int croppedHeight = texHeight - overlay.crop_top - overlay.crop_bottom;
        outW = static_cast<int>(croppedWidth * overlay.scale);
        outH = static_cast<int>(croppedHeight * overlay.scale);
    } else {
        outW = static_cast<int>(100 * overlay.scale);
        outH = static_cast<int>(100 * overlay.scale);
    }
}

static void CalculateWindowOverlayDimensions(const WindowOverlayConfig& overlay, int& outW, int& outH) {
    // Use try_lock to avoid blocking during hover detection
    std::unique_lock<std::mutex> lock(g_windowOverlayCacheMutex, std::try_to_lock);
    if (lock.owns_lock()) {
        CalculateWindowOverlayDimensionsUnsafe(overlay, outW, outH);
    } else {
        // Default size if mutex is busy
        outW = static_cast<int>(100 * overlay.scale);
        outH = static_cast<int>(100 * overlay.scale);
    }
}

const char* solid_vert_shader = R"(#version 330 core
layout(location = 0) in vec2 aPos;
void main() {
    gl_Position = vec4(aPos.x, aPos.y, 0.0, 1.0);
})";

const char* passthrough_vert_shader = R"(#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;
out vec2 TexCoord;
void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    TexCoord = aTexCoord;
})";

const char* filter_vert_shader = R"(#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;
out vec2 TexCoord;
uniform vec4 u_sourceRect;

void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    TexCoord = u_sourceRect.xy + aTexCoord * u_sourceRect.zw;
})";

const char* filter_frag_shader = R"(#version 330 core
out vec4 FragColor;
in vec2 TexCoord;
uniform sampler2D screenTexture;
uniform vec3 targetColor;
uniform vec3 outputColor;
uniform float u_sensitivity;

void main() {
    vec3 screenColorSRGB = texture(screenTexture, TexCoord).rgb;
    vec3 screenColorLinear = pow(screenColorSRGB, vec3(2.2));
    vec3 targetColorLinear = pow(targetColor, vec3(2.2));

    if (distance(screenColorLinear, targetColorLinear) < u_sensitivity) {
        FragColor = vec4(outputColor, 1.0);
    } else {
        FragColor = vec4(0.0, 0.0, 0.0, 0.0);
    }
})";

const char* render_frag_shader = R"(#version 330 core
    out vec4 FragColor;
    in vec2 TexCoord;

    uniform sampler2D filterTexture;
    uniform int u_borderWidth;
    uniform vec3 u_outputColor;
    uniform vec3 u_borderColor;
    uniform vec2 u_screenPixel;

    void main() {
        float centerAlpha = texture(filterTexture, TexCoord).a;

        if (centerAlpha > 0.5) {
            FragColor = vec4(u_outputColor, 1.0);
            return;
        }

        
        for (int x = -u_borderWidth; x <= u_borderWidth; x++) {
            for (int y = -u_borderWidth; y <= u_borderWidth; y++) {
                
                if (x == 0 && y == 0) continue;

                vec2 offset = vec2(float(x), float(y)) * u_screenPixel;
                float alpha = texture(filterTexture, TexCoord + offset).a;

                if (alpha > 0.5) {
                    FragColor = vec4(u_borderColor, 1.0);
                    return;
                }
            }
        }

        discard;
    })";

const char* background_frag_shader = R"(#version 330 core
out vec4 FragColor;
in vec2 TexCoord;
uniform sampler2D backgroundTexture;
uniform float u_opacity;
void main() {
    vec4 texColor = texture(backgroundTexture, TexCoord);
    FragColor = vec4(texColor.rgb, texColor.a * u_opacity);
})";

const char* solid_color_frag_shader = R"(#version 330 core
out vec4 FragColor;
uniform vec4 u_color;
void main() {
    FragColor = u_color;
})";

const char* image_render_frag_shader = R"(#version 330 core
out vec4 FragColor;
in vec2 TexCoord;

uniform sampler2D imageTexture;
uniform bool u_enableColorKey;
uniform vec3 u_colorKey;
uniform float u_sensitivity;
uniform float u_opacity;

void main() {
    vec4 texColor = texture(imageTexture, TexCoord);

    if (u_enableColorKey) {
        vec3 linearTexColor = pow(texColor.rgb, vec3(2.2));
        vec3 linearKeyColor = pow(u_colorKey, vec3(2.2));
        float dist = distance(linearTexColor, linearKeyColor);
        if (dist < u_sensitivity) {
            discard;
        }
    }
    
    FragColor = vec4(texColor.rgb, texColor.a * u_opacity);
})";

const char* passthrough_frag_shader = R"(#version 330 core
out vec4 FragColor;
in vec2 TexCoord;
uniform sampler2D screenTexture;

void main() {
    FragColor = texture(screenTexture, TexCoord);
})";

const char* gradient_frag_shader = R"(#version 330 core
out vec4 FragColor;
in vec2 TexCoord;

#define MAX_STOPS 8
#define ANIM_NONE 0
#define ANIM_ROTATE 1
#define ANIM_SLIDE 2
#define ANIM_WAVE 3
#define ANIM_SPIRAL 4
#define ANIM_FADE 5

uniform int u_numStops;
uniform vec4 u_stopColors[MAX_STOPS];
uniform float u_stopPositions[MAX_STOPS];
uniform float u_angle;
uniform float u_time;
uniform int u_animationType;
uniform float u_animationSpeed;
uniform bool u_colorFade;

vec4 getGradientColorSeamless(float t) {
    t = fract(t);
    
    
    float lastPos = u_stopPositions[u_numStops - 1];
    float firstPos = u_stopPositions[0];
    float wrapSize = (1.0 - lastPos) + firstPos;
    
    if (t <= firstPos && wrapSize > 0.001) {
        float wrapT = (firstPos - t) / wrapSize;
        return mix(u_stopColors[0], u_stopColors[u_numStops - 1], wrapT);
    }
    else if (t >= lastPos && wrapSize > 0.001) {
        float wrapT = (t - lastPos) / wrapSize;
        return mix(u_stopColors[u_numStops - 1], u_stopColors[0], wrapT);
    }
    
    vec4 color = u_stopColors[0];
    for (int i = 0; i < u_numStops - 1; i++) {
        if (t >= u_stopPositions[i] && t <= u_stopPositions[i + 1]) {
            float segmentT = (t - u_stopPositions[i]) / max(u_stopPositions[i + 1] - u_stopPositions[i], 0.0001);
            color = mix(u_stopColors[i], u_stopColors[i + 1], segmentT);
            break;
        }
    }
    return color;
}

vec4 getGradientColor(float t, float timeOffset) {
    float adjustedT = t;
    if (u_colorFade) {
        adjustedT = fract(t + timeOffset * 0.1);
    }
    adjustedT = clamp(adjustedT, 0.0, 1.0);
    
    vec4 color = u_stopColors[0];
    for (int i = 0; i < u_numStops - 1; i++) {
        if (adjustedT >= u_stopPositions[i] && adjustedT <= u_stopPositions[i + 1]) {
            float segmentT = (adjustedT - u_stopPositions[i]) / max(u_stopPositions[i + 1] - u_stopPositions[i], 0.0001);
            color = mix(u_stopColors[i], u_stopColors[i + 1], segmentT);
            break;
        }
    }
    if (adjustedT >= u_stopPositions[u_numStops - 1]) {
        color = u_stopColors[u_numStops - 1];
    }
    return color;
}

vec4 getFadeColor(float timeOffset) {
    float cyclePos = fract(timeOffset * 0.1);
    
    vec4 color = u_stopColors[0];
    for (int i = 0; i < u_numStops - 1; i++) {
        if (cyclePos >= u_stopPositions[i] && cyclePos <= u_stopPositions[i + 1]) {
            float segmentT = (cyclePos - u_stopPositions[i]) / max(u_stopPositions[i + 1] - u_stopPositions[i], 0.0001);
            color = mix(u_stopColors[i], u_stopColors[i + 1], segmentT);
            break;
        }
    }
    if (cyclePos > u_stopPositions[u_numStops - 1]) {
        float wrapRange = 1.0 - u_stopPositions[u_numStops - 1] + u_stopPositions[0];
        float wrapT = (cyclePos - u_stopPositions[u_numStops - 1]) / max(wrapRange, 0.0001);
        color = mix(u_stopColors[u_numStops - 1], u_stopColors[0], wrapT);
    }
    else if (cyclePos < u_stopPositions[0]) {
        float wrapRange = 1.0 - u_stopPositions[u_numStops - 1] + u_stopPositions[0];
        float wrapT = (u_stopPositions[0] - cyclePos) / max(wrapRange, 0.0001);
        color = mix(u_stopColors[0], u_stopColors[u_numStops - 1], wrapT);
    }
    return color;
}

void main() {
    vec2 center = vec2(0.5, 0.5);
    vec2 uv = TexCoord - center;
    float effectiveAngle = u_angle;
    float t = 0.0;
    float timeOffset = u_time * u_animationSpeed;
    
    if (u_animationType == ANIM_NONE) {
        vec2 dir = vec2(cos(u_angle), sin(u_angle));
        t = dot(uv, dir) + 0.5;
        t = clamp(t, 0.0, 1.0);
        FragColor = getGradientColor(t, timeOffset);
    }
    else if (u_animationType == ANIM_ROTATE) {
        effectiveAngle = u_angle + timeOffset;
        vec2 dir = vec2(cos(effectiveAngle), sin(effectiveAngle));
        t = dot(uv, dir) + 0.5;
        t = clamp(t, 0.0, 1.0);
        FragColor = getGradientColor(t, timeOffset);
    }
    else if (u_animationType == ANIM_SLIDE) {
        vec2 dir = vec2(cos(u_angle), sin(u_angle));
        t = dot(uv, dir) + 0.5;
        t = t + timeOffset * 0.2;
        FragColor = getGradientColorSeamless(t);
    }
    else if (u_animationType == ANIM_WAVE) {
        vec2 dir = vec2(cos(u_angle), sin(u_angle));
        vec2 perpDir = vec2(-sin(u_angle), cos(u_angle));
        float perpPos = dot(uv, perpDir);
        float wave = sin(perpPos * 8.0 + timeOffset * 2.0) * 0.08;
        t = dot(uv, dir) + 0.5 + wave;
        t = clamp(t, 0.0, 1.0);
        FragColor = getGradientColor(t, timeOffset);
    }
    else if (u_animationType == ANIM_SPIRAL) {
        float dist = length(uv) * 2.0;
        float angle = atan(uv.y, uv.x);
        t = dist + angle / 6.28318 - timeOffset * 0.3;
        FragColor = getGradientColorSeamless(t);
    }
    else if (u_animationType == ANIM_FADE) {
        FragColor = getFadeColor(timeOffset);
    }
    else {
        t = clamp(t, 0.0, 1.0);
        FragColor = getGradientColor(t, timeOffset);
    }
})";

void InitializeShaders() {
    PROFILE_SCOPE_CAT("Shader Initialization", "GPU Operations");
    g_filterProgram = CreateShaderProgram(filter_vert_shader, filter_frag_shader);
    g_renderProgram = CreateShaderProgram(passthrough_vert_shader, render_frag_shader);
    g_backgroundProgram = CreateShaderProgram(passthrough_vert_shader, background_frag_shader);
    g_solidColorProgram = CreateShaderProgram(solid_vert_shader, solid_color_frag_shader);
    g_imageRenderProgram = CreateShaderProgram(passthrough_vert_shader, image_render_frag_shader);
    g_passthroughProgram = CreateShaderProgram(filter_vert_shader, passthrough_frag_shader);
    g_gradientProgram = CreateShaderProgram(passthrough_vert_shader, gradient_frag_shader);

    if (!g_filterProgram || !g_renderProgram || !g_backgroundProgram || !g_solidColorProgram || !g_imageRenderProgram ||
        !g_passthroughProgram || !g_gradientProgram) {
        Log("FATAL: Failed to create one or more shader programs. Aborting shader initialization.");
        return;
    }

    g_filterShaderLocs.screenTexture = glGetUniformLocation(g_filterProgram, "screenTexture");
    g_filterShaderLocs.targetColor = glGetUniformLocation(g_filterProgram, "targetColor");
    g_filterShaderLocs.outputColor = glGetUniformLocation(g_filterProgram, "outputColor");
    g_filterShaderLocs.sensitivity = glGetUniformLocation(g_filterProgram, "u_sensitivity");
    g_filterShaderLocs.sourceRect = glGetUniformLocation(g_filterProgram, "u_sourceRect");

    g_renderShaderLocs.filterTexture = glGetUniformLocation(g_renderProgram, "filterTexture");
    g_renderShaderLocs.borderWidth = glGetUniformLocation(g_renderProgram, "u_borderWidth");
    g_renderShaderLocs.outputColor = glGetUniformLocation(g_renderProgram, "u_outputColor");
    g_renderShaderLocs.borderColor = glGetUniformLocation(g_renderProgram, "u_borderColor");
    g_renderShaderLocs.screenPixel = glGetUniformLocation(g_renderProgram, "u_screenPixel");

    g_backgroundShaderLocs.backgroundTexture = glGetUniformLocation(g_backgroundProgram, "backgroundTexture");
    g_backgroundShaderLocs.opacity = glGetUniformLocation(g_backgroundProgram, "u_opacity");

    g_solidColorShaderLocs.color = glGetUniformLocation(g_solidColorProgram, "u_color");

    g_imageRenderShaderLocs.imageTexture = glGetUniformLocation(g_imageRenderProgram, "imageTexture");
    g_imageRenderShaderLocs.enableColorKey = glGetUniformLocation(g_imageRenderProgram, "u_enableColorKey");
    g_imageRenderShaderLocs.colorKey = glGetUniformLocation(g_imageRenderProgram, "u_colorKey");
    g_imageRenderShaderLocs.sensitivity = glGetUniformLocation(g_imageRenderProgram, "u_sensitivity");
    g_imageRenderShaderLocs.opacity = glGetUniformLocation(g_imageRenderProgram, "u_opacity");

    g_passthroughShaderLocs.screenTexture = glGetUniformLocation(g_passthroughProgram, "screenTexture");
    g_passthroughShaderLocs.sourceRect = glGetUniformLocation(g_passthroughProgram, "u_sourceRect");

    g_gradientShaderLocs.numStops = glGetUniformLocation(g_gradientProgram, "u_numStops");
    g_gradientShaderLocs.stopColors = glGetUniformLocation(g_gradientProgram, "u_stopColors");
    g_gradientShaderLocs.stopPositions = glGetUniformLocation(g_gradientProgram, "u_stopPositions");
    g_gradientShaderLocs.angle = glGetUniformLocation(g_gradientProgram, "u_angle");
    g_gradientShaderLocs.time = glGetUniformLocation(g_gradientProgram, "u_time");
    g_gradientShaderLocs.animationType = glGetUniformLocation(g_gradientProgram, "u_animationType");
    g_gradientShaderLocs.animationSpeed = glGetUniformLocation(g_gradientProgram, "u_animationSpeed");
    g_gradientShaderLocs.colorFade = glGetUniformLocation(g_gradientProgram, "u_colorFade");

    glUseProgram(g_renderProgram);
    glUniform1i(g_renderShaderLocs.filterTexture, 0);

    glUseProgram(g_backgroundProgram);
    glUniform1i(g_backgroundShaderLocs.backgroundTexture, 0);

    glUseProgram(g_imageRenderProgram);
    glUniform1i(g_imageRenderShaderLocs.imageTexture, 0);

    glUseProgram(g_filterProgram);
    glUniform1i(g_filterShaderLocs.screenTexture, 0);

    glUseProgram(g_passthroughProgram);
    glUniform1i(g_passthroughShaderLocs.screenTexture, 0);

    glUseProgram(0);

}

void CleanupShaders() {
    if (g_filterProgram) {
        glDeleteProgram(g_filterProgram);
        g_filterProgram = 0;
    }
    if (g_renderProgram) {
        glDeleteProgram(g_renderProgram);
        g_renderProgram = 0;
    }
    if (g_backgroundProgram) {
        glDeleteProgram(g_backgroundProgram);
        g_backgroundProgram = 0;
    }
    if (g_solidColorProgram) {
        glDeleteProgram(g_solidColorProgram);
        g_solidColorProgram = 0;
    }
    if (g_imageRenderProgram) {
        glDeleteProgram(g_imageRenderProgram);
        g_imageRenderProgram = 0;
    }
    if (g_passthroughProgram) {
        glDeleteProgram(g_passthroughProgram);
        g_passthroughProgram = 0;
    }
    if (g_gradientProgram) {
        glDeleteProgram(g_gradientProgram);
        g_gradientProgram = 0;
    }
}

void DiscardAllGPUImages() {
    PROFILE_SCOPE_CAT("GPU Image Discard", "GPU Operations");
    std::vector<GLuint> texturesToDelete;

    {
        std::lock_guard<std::mutex> bgLock(g_backgroundTexturesMutex);
        for (auto const& [id, inst] : g_backgroundTextures) {
            if (inst.isAnimated) {
                for (GLuint tex : inst.frameTextures) {
                    if (tex != 0) texturesToDelete.push_back(tex);
                }
            } else if (inst.textureId != 0) {
                texturesToDelete.push_back(inst.textureId);
            }
        }
        g_backgroundTextures.clear();
    }

    {
        std::lock_guard<std::mutex> imageLock(g_userImagesMutex);
        for (auto const& [id, inst] : g_userImages) {
            if (inst.isAnimated) {
                for (GLuint tex : inst.frameTextures) {
                    if (tex != 0) texturesToDelete.push_back(tex);
                }
            } else if (inst.textureId != 0) {
                texturesToDelete.push_back(inst.textureId);
            }
        }
        g_userImages.clear();
    }

    // Enqueue for deletion after releasing resource-map locks.
    {
        std::lock_guard<std::mutex> lock(g_texturesToDeleteMutex);
        g_texturesToDelete.insert(g_texturesToDelete.end(), texturesToDelete.begin(), texturesToDelete.end());
    }
    if (!texturesToDelete.empty()) { g_hasTexturesToDelete.store(true, std::memory_order_release); }
    Log("All background and user image textures have been queued for deletion.");
}

void SaveGLState(GLState* s) {
    glGetIntegerv(GL_CURRENT_PROGRAM, &s->p);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &s->va);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &s->ab);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &s->read_fb);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &s->draw_fb);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &s->at);

    glGetIntegerv(GL_TEXTURE_BINDING_2D, &s->t);
    if (s->at == GL_TEXTURE0) {
        s->t0 = s->t;
    } else {
        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &s->t0);
        glActiveTexture(s->at);
    }

    s->fb = s->draw_fb;

    s->be = glIsEnabled(GL_BLEND);
    s->de = glIsEnabled(GL_DEPTH_TEST);
    s->sc = glIsEnabled(GL_SCISSOR_TEST);
    s->srgb_enabled = glIsEnabled(GL_FRAMEBUFFER_SRGB);

    glGetIntegerv(GL_BLEND_SRC_RGB, &s->blend_src_rgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &s->blend_dst_rgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &s->blend_src_alpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &s->blend_dst_alpha);

    glGetIntegerv(GL_VIEWPORT, &s->vp[0]);
    glGetIntegerv(GL_SCISSOR_BOX, s->sb);

    glGetFloatv(GL_COLOR_CLEAR_VALUE, s->cc);
    glGetFloatv(GL_LINE_WIDTH, &s->lw);
    glGetBooleanv(GL_COLOR_WRITEMASK, s->color_mask);
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &s->unpack_row_length);
    glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &s->unpack_skip_pixels);
    glGetIntegerv(GL_UNPACK_SKIP_ROWS, &s->unpack_skip_rows);
    glGetIntegerv(GL_PACK_ALIGNMENT, &s->pack_alignment);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &s->unpack_alignment);
}

void RestoreGLState(const GLState& s) {
    glUseProgram(s.p);
    glBindVertexArray(s.va);
    glBindBuffer(GL_ARRAY_BUFFER, s.ab);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, s.read_fb);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s.draw_fb);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s.t0);
    if (s.at != GL_TEXTURE0) {
        glActiveTexture(s.at);
        glBindTexture(GL_TEXTURE_2D, s.t);
    } else {
        glActiveTexture(s.at);
    }

    if (s.be)
        glEnable(GL_BLEND);
    else
        glDisable(GL_BLEND);
    if (s.de)
        glEnable(GL_DEPTH_TEST);
    else
        glDisable(GL_DEPTH_TEST);
    if (s.sc)
        glEnable(GL_SCISSOR_TEST);
    else
        glDisable(GL_SCISSOR_TEST);
    if (s.srgb_enabled)
        glEnable(GL_FRAMEBUFFER_SRGB);
    else
        glDisable(GL_FRAMEBUFFER_SRGB);

    glBlendFuncSeparate(s.blend_src_rgb, s.blend_dst_rgb, s.blend_src_alpha, s.blend_dst_alpha);

    if (oglViewport)
        oglViewport(s.vp[0], s.vp[1], s.vp[2], s.vp[3]);
    else
        glViewport(s.vp[0], s.vp[1], s.vp[2], s.vp[3]);
    glScissor(s.sb[0], s.sb[1], s.sb[2], s.sb[3]);

    glClearColor(s.cc[0], s.cc[1], s.cc[2], s.cc[3]);
    glLineWidth(s.lw);
    glColorMask(s.color_mask[0], s.color_mask[1], s.color_mask[2], s.color_mask[3]);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, s.unpack_row_length);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, s.unpack_skip_pixels);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, s.unpack_skip_rows);
    glPixelStorei(GL_PACK_ALIGNMENT, s.pack_alignment);
    glPixelStorei(GL_UNPACK_ALIGNMENT, s.unpack_alignment);
}

void CleanupGPUResources() {
    Log("CleanupGPUResources: Starting cleanup...");

    if (g_mirrorCaptureRunning.load(std::memory_order_acquire)) {
        StopMirrorCaptureThread();
    }

    HGLRC currentContext = wglGetCurrentContext();
    if (!currentContext) {
        Log("CleanupGPUResources: WARNING - No current GL context, cannot perform GPU cleanup");
        return;
    }

    // Lock all GPU resource mutexes during cleanup
    std::unique_lock<std::shared_mutex> mirrorLock(g_mirrorInstancesMutex); // Write lock - cleanup
    std::lock_guard<std::mutex> imageLock(g_userImagesMutex);
    std::lock_guard<std::mutex> bgLock(g_backgroundTexturesMutex);

    auto safeGLDelete = [](auto deleteFunc, auto handle) {
        if (handle == 0) return;
        try {
            deleteFunc(handle);
            while (glGetError() != GL_NO_ERROR) {}
        } catch (...) { Log("Exception during GPU resource deletion"); }
    };

    // PBO system cleanup is handled by CleanupCapturePBOs() in mirror_thread.cpp
    // No fence cleanup needed here since captureFence was removed from MirrorInstance

    try {
        for (auto const& [k, v] : g_mirrorInstances) {
            if (v.fbo) {
                glDeleteFramebuffers(1, &v.fbo);
                while (glGetError() != GL_NO_ERROR) {}
            }
            // Clean up back-buffer FBO used by capture thread
            if (v.fboBack) {
                glDeleteFramebuffers(1, &v.fboBack);
                while (glGetError() != GL_NO_ERROR) {}
            }
            if (v.finalFbo) {
                glDeleteFramebuffers(1, &v.finalFbo);
                while (glGetError() != GL_NO_ERROR) {}
            }
            if (v.finalFboBack) {
                glDeleteFramebuffers(1, &v.finalFboBack);
                while (glGetError() != GL_NO_ERROR) {}
            }
        }
        if (g_sceneFBO) {
            glDeleteFramebuffers(1, &g_sceneFBO);
            while (glGetError() != GL_NO_ERROR) {}
            g_sceneFBO = 0;
        }
    } catch (...) { Log("CleanupGPUResources: Exception during FBO cleanup"); }

    try {
        for (auto const& [k, v] : g_mirrorInstances) {
            if (v.fboTexture) {
                glDeleteTextures(1, &v.fboTexture);
                while (glGetError() != GL_NO_ERROR) {}
            }
            // Clean up back-buffer texture used by capture thread
            if (v.fboTextureBack) {
                glDeleteTextures(1, &v.fboTextureBack);
                while (glGetError() != GL_NO_ERROR) {}
            }
            if (v.finalTexture) {
                glDeleteTextures(1, &v.finalTexture);
                while (glGetError() != GL_NO_ERROR) {}
            }
            if (v.finalTextureBack) {
                glDeleteTextures(1, &v.finalTextureBack);
                while (glGetError() != GL_NO_ERROR) {}
            }

            // Clean up GPU sync fences
            if (v.gpuFence && glIsSync(v.gpuFence)) { glDeleteSync(v.gpuFence); }
            if (v.gpuFenceBack && glIsSync(v.gpuFenceBack)) { glDeleteSync(v.gpuFenceBack); }
        }
        g_mirrorInstances.clear();

        if (g_sceneTexture) {
            glDeleteTextures(1, &g_sceneTexture);
            while (glGetError() != GL_NO_ERROR) {}
            g_sceneTexture = 0;
        }

        DiscardAllGPUImages();

        {
            std::lock_guard<std::mutex> lock(g_texturesToDeleteMutex);
            if (!g_texturesToDelete.empty()) {
                glDeleteTextures((GLsizei)g_texturesToDelete.size(), g_texturesToDelete.data());
                while (glGetError() != GL_NO_ERROR) {}
                g_texturesToDelete.clear();
            }
        }
    } catch (...) { Log("CleanupGPUResources: Exception during texture cleanup"); }

    {
        std::lock_guard<std::mutex> lock(g_decodedImagesMutex);
        if (!g_decodedImagesQueue.empty()) {
            Log("Cleaning up " + std::to_string(g_decodedImagesQueue.size()) + " " + "pending decoded images to prevent memory leaks...");
            for (auto& decodedImg : g_decodedImagesQueue) {
                if (decodedImg.data) {
                    stbi_image_free(decodedImg.data);
                    decodedImg.data = nullptr;
                }
            }
            g_decodedImagesQueue.clear();
        }
    }

    try {
        if (g_vao) {
            glDeleteVertexArrays(1, &g_vao);
            while (glGetError() != GL_NO_ERROR) {}
            g_vao = 0;
        }
        if (g_vbo) {
            glDeleteBuffers(1, &g_vbo);
            while (glGetError() != GL_NO_ERROR) {}
            g_vbo = 0;
        }
        if (g_debugVAO) {
            glDeleteVertexArrays(1, &g_debugVAO);
            while (glGetError() != GL_NO_ERROR) {}
            g_debugVAO = 0;
        }
        if (g_debugVBO) {
            glDeleteBuffers(1, &g_debugVBO);
            while (glGetError() != GL_NO_ERROR) {}
            g_debugVBO = 0;
        }
    } catch (...) { Log("CleanupGPUResources: Exception during VAO/VBO cleanup"); }

    try {
        CleanupShaders();
        while (glGetError() != GL_NO_ERROR) {}
    } catch (...) { Log("CleanupGPUResources: Exception during shader cleanup"); }

    g_sceneW = g_sceneH = 0;
    g_glInitialized.store(false, std::memory_order_release);
    Log("CleanupGPUResources: Cleanup complete.");
}
void UploadDecodedImageToGPU(const DecodedImageData& imgData) {
    PROFILE_SCOPE_CAT("GPU Image Upload", "GPU Operations");
    if (imgData.type == DecodedImageData::Type::Background) {
        std::lock_guard<std::mutex> bgLock(g_backgroundTexturesMutex);

        auto it = g_backgroundTextures.find(imgData.id);
        if (it != g_backgroundTextures.end()) {
            BackgroundTextureInstance& oldInst = it->second;
            std::lock_guard<std::mutex> lock(g_texturesToDeleteMutex);
            if (oldInst.isAnimated) {
                for (GLuint tex : oldInst.frameTextures) {
                    if (tex != 0) g_texturesToDelete.push_back(tex);
                }
            } else if (oldInst.textureId != 0) {
                g_texturesToDelete.push_back(oldInst.textureId);
            }
            g_hasTexturesToDelete.store(true, std::memory_order_release);
            g_backgroundTextures.erase(it);
        }

        if (imgData.data) {
            BackgroundTextureInstance inst;

            if (imgData.isAnimated && imgData.frameCount > 1) {
                inst.isAnimated = true;
                inst.frameDelays = imgData.frameDelays;
                inst.currentFrame = 0;
                inst.lastFrameTime = std::chrono::steady_clock::now();

                int frameHeight = imgData.frameHeight;
                for (int i = 0; i < imgData.frameCount; i++) {
                    GLuint t;
                    glGenTextures(1, &t);
                    glBindTexture(GL_TEXTURE_2D, t);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
                    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
                    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

                    unsigned char* frameData = imgData.data + (i * frameHeight * imgData.width * 4);
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, imgData.width, frameHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, frameData);
                    glGenerateMipmap(GL_TEXTURE_2D);

                    inst.frameTextures.push_back(t);
                }
                inst.textureId = inst.frameTextures[0];

                g_backgroundTextures[imgData.id] = inst;
                Log("Uploaded animated background for '" + imgData.id + "' to GPU (" + std::to_string(imgData.frameCount) + " frames).");
            } else {
                inst.isAnimated = false;

                GLuint t;
                glGenTextures(1, &t);
                glBindTexture(GL_TEXTURE_2D, t);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
                glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, imgData.width, imgData.frameHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, imgData.data);
                glGenerateMipmap(GL_TEXTURE_2D);

                inst.textureId = t;
                g_backgroundTextures[imgData.id] = inst;
                Log("Uploaded background for '" + imgData.id + "' to GPU.");
            }
        } else {
            Log("Skipping GPU upload for background '" + imgData.id + "' due to null image data.");
        }
    } else if (imgData.type == DecodedImageData::Type::UserImage) {
        // Remove old instance under lock, but avoid holding the lock while uploading new textures.
        UserImageInstance oldInst;
        bool hadOldInst = false;
        {
            std::lock_guard<std::mutex> imageLock(g_userImagesMutex);
            auto it = g_userImages.find(imgData.id);
            if (it != g_userImages.end()) {
                oldInst = std::move(it->second);
                g_userImages.erase(it);
                hadOldInst = true;
            }
        }
        if (hadOldInst) {
            std::lock_guard<std::mutex> lock(g_texturesToDeleteMutex);
            if (oldInst.isAnimated) {
                for (GLuint tex : oldInst.frameTextures) {
                    if (tex != 0) g_texturesToDelete.push_back(tex);
                }
            } else if (oldInst.textureId != 0) {
                g_texturesToDelete.push_back(oldInst.textureId);
            }
        }
        if (hadOldInst) { g_hasTexturesToDelete.store(true, std::memory_order_release); }

        if (imgData.data) {
            UserImageInstance inst;
            inst.width = imgData.width;
            inst.height = imgData.frameHeight;

            inst.isFullyTransparent = true;
            int framePixels = imgData.width * imgData.frameHeight;
            for (int i = 0; i < framePixels; i++) {
                if (imgData.data[i * 4 + 3] > 0) {
                    inst.isFullyTransparent = false;
                    break;
                }
            }

            if (imgData.isAnimated && imgData.frameCount > 1) {
                inst.isAnimated = true;
                inst.frameDelays = imgData.frameDelays;
                inst.currentFrame = 0;
                inst.lastFrameTime = std::chrono::steady_clock::now();

                int frameHeight = imgData.frameHeight;
                for (int i = 0; i < imgData.frameCount; i++) {
                    GLuint t;
                    glGenTextures(1, &t);
                    glBindTexture(GL_TEXTURE_2D, t);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
                    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
                    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

                    unsigned char* frameData = imgData.data + (i * frameHeight * imgData.width * 4);
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, imgData.width, frameHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, frameData);
                    glGenerateMipmap(GL_TEXTURE_2D);

                    inst.frameTextures.push_back(t);
                }
                inst.textureId = inst.frameTextures[0];

                {
                    std::lock_guard<std::mutex> imageLock(g_userImagesMutex);
                    g_userImages[imgData.id] = std::move(inst);
                }
                Log("Uploaded animated user image '" + imgData.id + "' to GPU (" + std::to_string(imgData.frameCount) + " frames).");
            } else {
                inst.isAnimated = false;

                glGenTextures(1, &inst.textureId);
                glBindTexture(GL_TEXTURE_2D, inst.textureId);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
                glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, imgData.width, imgData.frameHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, imgData.data);
                glGenerateMipmap(GL_TEXTURE_2D);

                {
                    std::lock_guard<std::mutex> imageLock(g_userImagesMutex);
                    g_userImages[imgData.id] = std::move(inst);
                }
                Log("Uploaded user image '" + imgData.id + "' to GPU.");
            }
        } else {
            Log("Skipping GPU upload for user image '" + imgData.id + "' due to null image data.");
        }
    }
}

void InitializeGPUResources() {
    PROFILE_SCOPE_CAT("GPU Resource Initialization", "GPU Operations");

    GLint last_program;
    glGetIntegerv(GL_CURRENT_PROGRAM, &last_program);
    GLint last_texture;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &last_texture);
    GLint last_active_texture;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &last_active_texture);
    GLint last_array_buffer;
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &last_array_buffer);
    GLint last_vertex_array;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &last_vertex_array);
    GLint last_framebuffer;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &last_framebuffer);

    CleanupGPUResources();

    // This prevents potential race conditions with g_configMutex during startup

    if (g_configLoadFailed.load()) {
        Log("FATAL: Config load failed. Aborting GPU resource initialization.");
        return;
    }

    InitializeShaders();

    if (!g_filterProgram || !g_renderProgram || !g_backgroundProgram || !g_solidColorProgram || !g_imageRenderProgram ||
        !g_passthroughProgram) {
        Log("FATAL: Failed to create one or more shader programs. Aborting GPU resource initialization.");
        glBindFramebuffer(GL_FRAMEBUFFER, last_framebuffer);
        glBindVertexArray(last_vertex_array);
        glUseProgram(last_program);
        return;
    }

    g_pendingImageLoad = true;

    std::vector<MirrorConfig> mirrorsToCreate;
    {
        auto initSnap = GetConfigSnapshot();
        if (initSnap) { mirrorsToCreate = initSnap->mirrors; }
        LogCategory("init", "Found " + std::to_string(mirrorsToCreate.size()) + " mirrors in config to create.");
    }
    // Release the framebuffer binding before calling CreateMirrorGPUResources
    glBindFramebuffer(GL_FRAMEBUFFER, last_framebuffer);

    for (const auto& conf : mirrorsToCreate) {
        CreateMirrorGPUResources(conf);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, last_framebuffer);
    glGenVertexArrays(1, &g_vao);
    glGenBuffers(1, &g_vbo);
    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 192, nullptr, GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glGenVertexArrays(1, &g_debugVAO);
    glGenBuffers(1, &g_debugVBO);
    glBindVertexArray(g_debugVAO);
    glBindBuffer(GL_ARRAY_BUFFER, g_debugVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 4 * 2, nullptr, GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    static const float fullscreenQuadVerts[] = {
        -1.0f, -1.0f, 0.0f, 0.0f,
        1.0f,  -1.0f, 1.0f, 0.0f,
        1.0f,  1.0f,  1.0f, 1.0f,
        -1.0f, -1.0f, 0.0f, 0.0f,
        1.0f,  1.0f,  1.0f, 1.0f,
        -1.0f, 1.0f,  0.0f, 1.0f
    };
    glGenVertexArrays(1, &g_fullscreenQuadVAO);
    glGenBuffers(1, &g_fullscreenQuadVBO);
    glBindVertexArray(g_fullscreenQuadVAO);
    glBindBuffer(GL_ARRAY_BUFFER, g_fullscreenQuadVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(fullscreenQuadVerts), fullscreenQuadVerts, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glBindVertexArray(0);

    LogCategory("init", "Restoring original OpenGL state...");
    glUseProgram(last_program);
    glActiveTexture(last_active_texture);
    glBindTexture(GL_TEXTURE_2D, last_texture);
    glBindVertexArray(last_vertex_array);
    glBindBuffer(GL_ARRAY_BUFFER, last_array_buffer);
    glBindFramebuffer(GL_FRAMEBUFFER, last_framebuffer);

    g_glInitialized.store(true, std::memory_order_release);
    LogCategory("init", "--- GPU resources initialized successfully. ---");
}

void CreateMirrorGPUResources(const MirrorConfig& conf) {
    PROFILE_SCOPE_CAT("Create Mirror GPU Resources", "GPU Operations");

    if (conf.input.empty()) {
        Log("Warning: Mirror '" + conf.name + "' has no input regions. Skipping GPU resource creation.");
        return;
    }

    // Lock mutex before accessing g_mirrorInstances
    std::unique_lock<std::shared_mutex> lock(g_mirrorInstancesMutex); // Write lock - creating instance

    auto it = g_mirrorInstances.find(conf.name);
    if (it != g_mirrorInstances.end()) {
        Log("Mirror '" + conf.name + "' GPU resources already exist. Skipping creation.");
        return;
    }

    GLint last_framebuffer;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &last_framebuffer);
    GLint last_texture;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &last_texture);

    MirrorInstance inst;
    int padding = (conf.border.type == MirrorBorderType::Dynamic) ? conf.border.dynamicThickness : 0;
    inst.fbo_w = conf.captureWidth + 2 * padding;
    inst.fbo_h = conf.captureHeight + 2 * padding;

    auto createFBO = [&](GLuint& fbo, GLuint& texture, int w, int h, GLenum filter) -> bool {
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
        return (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    };

    bool frontComplete = createFBO(inst.fbo, inst.fboTexture, inst.fbo_w, inst.fbo_h, GL_NEAREST);
    bool backComplete = createFBO(inst.fboBack, inst.fboTextureBack, inst.fbo_w, inst.fbo_h, GL_NEAREST);

    // Create final (screen-ready) FBOs - capture thread renders with borders here
    float scaleX = conf.output.separateScale ? conf.output.scaleX : conf.output.scale;
    float scaleY = conf.output.separateScale ? conf.output.scaleY : conf.output.scale;
    inst.final_w = static_cast<int>(inst.fbo_w * scaleX);
    inst.final_h = static_cast<int>(inst.fbo_h * scaleY);
    inst.final_w_back = inst.final_w;
    inst.final_h_back = inst.final_h;

    bool finalFrontComplete = createFBO(inst.finalFbo, inst.finalTexture, inst.final_w, inst.final_h, GL_NEAREST);
    bool finalBackComplete = createFBO(inst.finalFboBack, inst.finalTextureBack, inst.final_w, inst.final_h, GL_NEAREST);

    if (frontComplete && backComplete && finalFrontComplete && finalBackComplete) {
        inst.captureReady.store(false, std::memory_order_relaxed);
        inst.hasValidContent = false;
        // Initialize rawOutput state from config for proper initial synchronization
        inst.desiredRawOutput.store(conf.rawOutput, std::memory_order_relaxed);
        inst.capturedAsRawOutput = conf.rawOutput;
        inst.capturedAsRawOutputBack = conf.rawOutput;
        g_mirrorInstances[conf.name] = inst;
        LogCategory("init", "Created double-buffered GPU resources for mirror '" + conf.name + "' (FBO: " + std::to_string(inst.fbo) +
                                ", Back: " + std::to_string(inst.fboBack) + ", FinalFBO: " + std::to_string(inst.finalFbo) + " [" +
                                std::to_string(inst.final_w) + "x" + std::to_string(inst.final_h) + "])");
    } else {
        Log("ERROR: Failed to create complete framebuffers for mirror '" + conf.name + "'");
        if (inst.fboTexture) glDeleteTextures(1, &inst.fboTexture);
        if (inst.fbo) glDeleteFramebuffers(1, &inst.fbo);
        if (inst.fboTextureBack) glDeleteTextures(1, &inst.fboTextureBack);
        if (inst.fboBack) glDeleteFramebuffers(1, &inst.fboBack);
        if (inst.finalTexture) glDeleteTextures(1, &inst.finalTexture);
        if (inst.finalFbo) glDeleteFramebuffers(1, &inst.finalFbo);
        if (inst.finalTextureBack) glDeleteTextures(1, &inst.finalTextureBack);
        if (inst.finalFboBack) glDeleteFramebuffers(1, &inst.finalFboBack);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, last_framebuffer);
    glBindTexture(GL_TEXTURE_2D, last_texture);
}

// MirrorRenderData struct is now defined in render.h for sharing with render_thread.cpp

// All overlay rendering is now done asynchronously via the render thread.
// See RT_RenderMirrors() and RT_RenderImages() in render_thread.cpp

void handleEyeZoomMode(const GLState& s, float opacity, int animatedViewportX) {
    PROFILE_SCOPE_CAT("EyeZoom Mode Rendering", "Rendering");

    if (opacity <= 0.0f) { return; }

    EyeZoomConfig zoomConfig;
    {
        auto ezSnap = GetConfigSnapshot();
        if (ezSnap) { zoomConfig = ezSnap->eyezoom; }
    }

    const int fullW = GetCachedWindowWidth();
    const int fullH = GetCachedWindowHeight();

    bool useSnapshot = g_isTransitioningFromEyeZoom.load(std::memory_order_acquire);

    GLuint gameTextureToUse = g_cachedGameTextureId.load();

    if (useSnapshot && !s_eyeZoomSnapshotValid) {
        return;
    }

    if (!useSnapshot && gameTextureToUse == UINT_MAX) {
        return;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (oglViewport)
        oglViewport(0, 0, fullW, fullH);
    else
        glViewport(0, 0, fullW, fullH);
    glDisable(GL_FRAMEBUFFER_SRGB);
    glDisable(GL_SCISSOR_TEST);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);


    int modeWidth = zoomConfig.windowWidth;
    int targetViewportX = (fullW - modeWidth) / 2;

    int viewportX;
    if (animatedViewportX >= 0) {
        viewportX = animatedViewportX;
    } else {
        viewportX = targetViewportX;
    }

    int zoomOutputWidth = 0;
    int zoomOutputHeight = 0;
    int zoomX = 0;
    int zoomY = 0;

    if (zoomConfig.useCustomSizePosition) {
        zoomOutputWidth = zoomConfig.zoomAreaWidth;
        zoomOutputHeight = zoomConfig.zoomAreaHeight;
        zoomX = zoomConfig.positionX;
        zoomY = zoomConfig.positionY;
    } else {
        int autoHorizontalMargin = 0;
        if (targetViewportX > 0) autoHorizontalMargin = targetViewportX / 10;

        zoomOutputWidth = viewportX - (2 * autoHorizontalMargin);
        int autoVerticalMargin = fullH / 8;
        zoomOutputHeight = fullH - (2 * autoVerticalMargin);

        zoomX = autoHorizontalMargin;
        zoomY = (fullH - zoomOutputHeight) / 2;
    }

    if (zoomOutputWidth > fullW) zoomOutputWidth = fullW;

    if (zoomOutputWidth <= 20) {
        return;
    }

    if (zoomOutputHeight > fullH) zoomOutputHeight = fullH;

    if (zoomOutputHeight < 1) { zoomOutputHeight = 1; }

    int maxZoomX = (std::max)(0, fullW - zoomOutputWidth);
    int maxZoomY = (std::max)(0, fullH - zoomOutputHeight);
    zoomX = (std::max)(0, (std::min)(zoomX, maxZoomX));
    zoomY = (std::max)(0, (std::min)(zoomY, maxZoomY));

    int zoomY_gl = fullH - zoomY - zoomOutputHeight;

    int texWidth = zoomConfig.windowWidth;
    int texHeight = zoomConfig.windowHeight;

    int srcCenterX = texWidth / 2;
    int srcLeft = srcCenterX - zoomConfig.cloneWidth / 2;
    int srcRight = srcCenterX + zoomConfig.cloneWidth / 2;

    int srcCenterY = texHeight / 2;
    int srcBottom = srcCenterY - zoomConfig.cloneHeight / 2;
    int srcTop = srcCenterY + zoomConfig.cloneHeight / 2;

    int dstLeft = zoomX;
    int dstRight = zoomX + zoomOutputWidth;
    int dstBottom = zoomY_gl;
    int dstTop = zoomY_gl + zoomOutputHeight;


    auto EnsureEyeZoomSnapshotAllocated = [&]() {
        if (s_eyeZoomSnapshotTexture == 0 || s_eyeZoomSnapshotWidth != zoomOutputWidth || s_eyeZoomSnapshotHeight != zoomOutputHeight) {
            if (s_eyeZoomSnapshotTexture != 0) { glDeleteTextures(1, &s_eyeZoomSnapshotTexture); }
            if (s_eyeZoomSnapshotFBO != 0) { glDeleteFramebuffers(1, &s_eyeZoomSnapshotFBO); }

            glGenTextures(1, &s_eyeZoomSnapshotTexture);
            glBindTexture(GL_TEXTURE_2D, s_eyeZoomSnapshotTexture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, zoomOutputWidth, zoomOutputHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

            glGenFramebuffers(1, &s_eyeZoomSnapshotFBO);
            glBindFramebuffer(GL_FRAMEBUFFER, s_eyeZoomSnapshotFBO);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_eyeZoomSnapshotTexture, 0);

            s_eyeZoomSnapshotWidth = zoomOutputWidth;
            s_eyeZoomSnapshotHeight = zoomOutputHeight;
            s_eyeZoomSnapshotValid = false;
        }
    };

    auto ForceOpaqueAlphaInCurrentDrawFbo = [&](int x, int y, int w, int h) {
        if (w <= 0 || h <= 0) { return; }

        GLboolean prevColorMask[4] = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };
        glGetBooleanv(GL_COLOR_WRITEMASK, prevColorMask);

        GLboolean prevScissorEnabled = glIsEnabled(GL_SCISSOR_TEST);
        GLint prevScissorBox[4] = { 0, 0, 0, 0 };
        if (prevScissorEnabled) { glGetIntegerv(GL_SCISSOR_BOX, prevScissorBox); }

        glEnable(GL_SCISSOR_TEST);
        glScissor(x, y, w, h);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        glColorMask(prevColorMask[0], prevColorMask[1], prevColorMask[2], prevColorMask[3]);
        if (prevScissorEnabled) {
            glScissor(prevScissorBox[0], prevScissorBox[1], prevScissorBox[2], prevScissorBox[3]);
        } else {
            glDisable(GL_SCISSOR_TEST);
        }
    };

    if (opacity < 1.0f) {
        if (s_eyeZoomTempTexture == 0 || s_eyeZoomTempWidth != zoomOutputWidth || s_eyeZoomTempHeight != zoomOutputHeight) {
            if (s_eyeZoomTempTexture != 0) { glDeleteTextures(1, &s_eyeZoomTempTexture); }
            if (s_eyeZoomTempFBO != 0) { glDeleteFramebuffers(1, &s_eyeZoomTempFBO); }

            glGenFramebuffers(1, &s_eyeZoomTempFBO);
            glGenTextures(1, &s_eyeZoomTempTexture);

            glBindTexture(GL_TEXTURE_2D, s_eyeZoomTempTexture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, zoomOutputWidth, zoomOutputHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

            glBindFramebuffer(GL_FRAMEBUFFER, s_eyeZoomTempFBO);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_eyeZoomTempTexture, 0);

            s_eyeZoomTempWidth = zoomOutputWidth;
            s_eyeZoomTempHeight = zoomOutputHeight;
        }

        glBindFramebuffer(GL_FRAMEBUFFER, s_eyeZoomTempFBO);
        if (oglViewport)
            oglViewport(0, 0, zoomOutputWidth, zoomOutputHeight);
        else
            glViewport(0, 0, zoomOutputWidth, zoomOutputHeight);

        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        if (useSnapshot) {
            if (s_eyeZoomBlitFBO == 0) { glGenFramebuffers(1, &s_eyeZoomBlitFBO); }
            glBindFramebuffer(GL_READ_FRAMEBUFFER, s_eyeZoomBlitFBO);
            glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_eyeZoomSnapshotTexture, 0);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_eyeZoomTempFBO);
            glBlitFramebuffer(0, 0, s_eyeZoomSnapshotWidth, s_eyeZoomSnapshotHeight, 0, 0, zoomOutputWidth, zoomOutputHeight,
                              GL_COLOR_BUFFER_BIT, GL_NEAREST);
        } else {
            if (s_eyeZoomBlitFBO == 0) { glGenFramebuffers(1, &s_eyeZoomBlitFBO); }
            glBindFramebuffer(GL_READ_FRAMEBUFFER, s_eyeZoomBlitFBO);
            glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gameTextureToUse, 0);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_eyeZoomTempFBO);
            glBlitFramebuffer(srcLeft, srcBottom, srcRight, srcTop, 0, 0, zoomOutputWidth, zoomOutputHeight, GL_COLOR_BUFFER_BIT, GL_NEAREST);

            EnsureEyeZoomSnapshotAllocated();
            glBindFramebuffer(GL_READ_FRAMEBUFFER, s_eyeZoomTempFBO);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_eyeZoomSnapshotFBO);
            glBlitFramebuffer(0, 0, zoomOutputWidth, zoomOutputHeight, 0, 0, s_eyeZoomSnapshotWidth, s_eyeZoomSnapshotHeight,
                              GL_COLOR_BUFFER_BIT, GL_NEAREST);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            s_eyeZoomSnapshotValid = true;
        }

        glBindFramebuffer(GL_FRAMEBUFFER, s_eyeZoomTempFBO);
        ForceOpaqueAlphaInCurrentDrawFbo(0, 0, zoomOutputWidth, zoomOutputHeight);

        glBindFramebuffer(GL_FRAMEBUFFER, s_eyeZoomTempFBO);
        if (oglViewport)
            oglViewport(0, 0, zoomOutputWidth, zoomOutputHeight);
        else
            glViewport(0, 0, zoomOutputWidth, zoomOutputHeight);

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glUseProgram(g_solidColorProgram);
        glBindVertexArray(g_vao);
        glBindBuffer(GL_ARRAY_BUFFER, g_vbo);

        bool useDefaultOverlay_obs1 = (zoomConfig.activeOverlayIndex < 0 ||
                                      zoomConfig.activeOverlayIndex >= (int)zoomConfig.overlays.size());
        if (useDefaultOverlay_obs1) {
            float pixelWidthOnScreen = zoomOutputWidth / (float)zoomConfig.cloneWidth;
            int labelsPerSide = zoomConfig.cloneWidth / 2;
            int overlayLabelsPerSide = zoomConfig.overlayWidth;
            if (overlayLabelsPerSide < 0) overlayLabelsPerSide = labelsPerSide;
            if (overlayLabelsPerSide > labelsPerSide) overlayLabelsPerSide = labelsPerSide;
            float centerY_local = zoomOutputHeight / 2.0f;

            float boxHeight;
            if (zoomConfig.linkRectToFont) {
                boxHeight = g_overlayTextFontSize * 1.2f;
            } else {
                boxHeight = static_cast<float>(zoomConfig.rectHeight);
            }

            std::vector<float> evenVerts, oddVerts;
            evenVerts.reserve(overlayLabelsPerSide * 6 * 4);
            oddVerts.reserve(overlayLabelsPerSide * 6 * 4);

            for (int xOffset = -overlayLabelsPerSide; xOffset <= overlayLabelsPerSide; xOffset++) {
                if (xOffset == 0) continue;

                int boxIndex = xOffset + labelsPerSide - (xOffset > 0 ? 1 : 0);
                float boxLeft = boxIndex * pixelWidthOnScreen;
                float boxRight = boxLeft + pixelWidthOnScreen;
                float boxBottom_local = centerY_local - boxHeight / 2.0f;
                float boxTop_local = centerY_local + boxHeight / 2.0f;

                float boxNdcLeft = (boxLeft / (float)zoomOutputWidth) * 2.0f - 1.0f;
                float boxNdcRight = (boxRight / (float)zoomOutputWidth) * 2.0f - 1.0f;
                float boxNdcBottom = (boxBottom_local / (float)zoomOutputHeight) * 2.0f - 1.0f;
                float boxNdcTop = (boxTop_local / (float)zoomOutputHeight) * 2.0f - 1.0f;

                auto& verts = (boxIndex % 2 == 0) ? evenVerts : oddVerts;
                float quad[] = {
                    boxNdcLeft, boxNdcBottom, 0, 0, boxNdcRight, boxNdcBottom, 0, 0, boxNdcRight, boxNdcTop, 0, 0,
                    boxNdcLeft, boxNdcBottom, 0, 0, boxNdcRight, boxNdcTop,    0, 0, boxNdcLeft,  boxNdcTop, 0, 0,
                };
                verts.insert(verts.end(), std::begin(quad), std::end(quad));

                int displayNumber = abs(xOffset);
                float numberCenterX = zoomX + boxLeft + pixelWidthOnScreen / 2.0f;
                float numberCenterY = zoomY + zoomOutputHeight / 2.0f;
                CacheEyeZoomTextLabel(displayNumber, numberCenterX, numberCenterY, zoomConfig.textColor);
            }

            if (!evenVerts.empty()) {
                glUniform4f(g_solidColorShaderLocs.color, zoomConfig.gridColor1.r, zoomConfig.gridColor1.g, zoomConfig.gridColor1.b, zoomConfig.gridColor1Opacity);
                glBufferSubData(GL_ARRAY_BUFFER, 0, evenVerts.size() * sizeof(float), evenVerts.data());
                glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(evenVerts.size() / 4));
            }
            if (!oddVerts.empty()) {
                glUniform4f(g_solidColorShaderLocs.color, zoomConfig.gridColor2.r, zoomConfig.gridColor2.g, zoomConfig.gridColor2.b, zoomConfig.gridColor2Opacity);
                glBufferSubData(GL_ARRAY_BUFFER, 0, oddVerts.size() * sizeof(float), oddVerts.data());
                glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(oddVerts.size() / 4));
            }
        }

        float centerX_local = zoomOutputWidth / 2.0f;
        float centerLineWidth = 2.0f;
        float lineLeft = centerX_local - centerLineWidth / 2.0f;
        float lineRight = centerX_local + centerLineWidth / 2.0f;

        float lineNdcLeft = (lineLeft / (float)zoomOutputWidth) * 2.0f - 1.0f;
        float lineNdcRight = (lineRight / (float)zoomOutputWidth) * 2.0f - 1.0f;
        float lineNdcBottom = -1.0f;
        float lineNdcTop = 1.0f;

        glUniform4f(g_solidColorShaderLocs.color, zoomConfig.centerLineColor.r, zoomConfig.centerLineColor.g, zoomConfig.centerLineColor.b,
                    zoomConfig.centerLineColorOpacity);

        float centerLineVerts[] = {
            lineNdcLeft, lineNdcBottom, 0, 0, lineNdcRight, lineNdcBottom, 0, 0, lineNdcRight, lineNdcTop, 0, 0,
            lineNdcLeft, lineNdcBottom, 0, 0, lineNdcRight, lineNdcTop,    0, 0, lineNdcLeft,  lineNdcTop, 0, 0,
        };
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(centerLineVerts), centerLineVerts);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (oglViewport)
            oglViewport(0, 0, fullW, fullH);
        else
            glViewport(0, 0, fullW, fullH);

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        glUseProgram(g_imageRenderProgram);
        glBindTexture(GL_TEXTURE_2D, s_eyeZoomTempTexture);
        glUniform1i(g_imageRenderShaderLocs.imageTexture, 0);
        glUniform1i(g_imageRenderShaderLocs.enableColorKey, 0);
        glUniform1f(g_imageRenderShaderLocs.opacity, opacity);

        float nx1 = (static_cast<float>(dstLeft) / fullW) * 2.0f - 1.0f;
        float ny1 = (static_cast<float>(dstBottom) / fullH) * 2.0f - 1.0f;
        float nx2 = (static_cast<float>(dstRight) / fullW) * 2.0f - 1.0f;
        float ny2 = (static_cast<float>(dstTop) / fullH) * 2.0f - 1.0f;

        float rv[] = { nx1, ny1, 0, 0, nx2, ny1, 1, 0, nx2, ny2, 1, 1, nx1, ny1, 0, 0, nx2, ny2, 1, 1, nx1, ny2, 0, 1 };
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(rv), rv);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    } else {
        glDisable(GL_BLEND);

        if (useSnapshot) {
            if (s_eyeZoomBlitFBO == 0) { glGenFramebuffers(1, &s_eyeZoomBlitFBO); }
            glBindFramebuffer(GL_READ_FRAMEBUFFER, s_eyeZoomBlitFBO);
            glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_eyeZoomSnapshotTexture, 0);

            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

            glBlitFramebuffer(0, 0, s_eyeZoomSnapshotWidth, s_eyeZoomSnapshotHeight, dstLeft, dstBottom, dstRight, dstTop,
                              GL_COLOR_BUFFER_BIT, GL_NEAREST);
        } else {

            if (s_eyeZoomBlitFBO == 0) { glGenFramebuffers(1, &s_eyeZoomBlitFBO); }
            glBindFramebuffer(GL_READ_FRAMEBUFFER, s_eyeZoomBlitFBO);
            glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gameTextureToUse, 0);

            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

            glBlitFramebuffer(srcLeft, srcBottom, srcRight, srcTop, dstLeft, dstBottom, dstRight, dstTop, GL_COLOR_BUFFER_BIT, GL_NEAREST);

            EnsureEyeZoomSnapshotAllocated();

            glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_eyeZoomSnapshotFBO);
            glBlitFramebuffer(dstLeft, dstBottom, dstRight, dstTop, 0, 0, s_eyeZoomSnapshotWidth, s_eyeZoomSnapshotHeight,
                              GL_COLOR_BUFFER_BIT, GL_NEAREST);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);

            s_eyeZoomSnapshotValid = true;
        }

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        ForceOpaqueAlphaInCurrentDrawFbo(dstLeft, dstBottom, zoomOutputWidth, zoomOutputHeight);

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glUseProgram(g_solidColorProgram);
        glBindVertexArray(g_vao);
        glBindBuffer(GL_ARRAY_BUFFER, g_vbo);

        bool useDefaultOverlay_obs2 = (zoomConfig.activeOverlayIndex < 0 ||
                                      zoomConfig.activeOverlayIndex >= (int)zoomConfig.overlays.size());
        if (useDefaultOverlay_obs2) {
            float pixelWidthOnScreen = zoomOutputWidth / (float)zoomConfig.cloneWidth;
            int labelsPerSide = zoomConfig.cloneWidth / 2;
            int overlayLabelsPerSide = zoomConfig.overlayWidth;
            if (overlayLabelsPerSide < 0) overlayLabelsPerSide = labelsPerSide;
            if (overlayLabelsPerSide > labelsPerSide) overlayLabelsPerSide = labelsPerSide;
            float centerY = zoomY + zoomOutputHeight / 2.0f;

            float boxHeight;
            if (zoomConfig.linkRectToFont) {
                boxHeight = g_overlayTextFontSize * 1.2f;
            } else {
                boxHeight = static_cast<float>(zoomConfig.rectHeight);
            }

            std::vector<float> evenVerts, oddVerts;
            evenVerts.reserve(overlayLabelsPerSide * 6 * 4);
            oddVerts.reserve(overlayLabelsPerSide * 6 * 4);

            for (int xOffset = -overlayLabelsPerSide; xOffset <= overlayLabelsPerSide; xOffset++) {
                if (xOffset == 0) continue;

                int boxIndex = xOffset + labelsPerSide - (xOffset > 0 ? 1 : 0);
                float boxLeft = zoomX + (boxIndex * pixelWidthOnScreen);
                float boxRight = boxLeft + pixelWidthOnScreen;
                float boxBottom = centerY - boxHeight / 2.0f;
                float boxTop = centerY + boxHeight / 2.0f;

                float boxNdcLeft = (boxLeft / (float)fullW) * 2.0f - 1.0f;
                float boxNdcRight = (boxRight / (float)fullW) * 2.0f - 1.0f;
                float boxNdcBottom = (boxBottom / (float)fullH) * 2.0f - 1.0f;
                float boxNdcTop = (boxTop / (float)fullH) * 2.0f - 1.0f;

                auto& verts = (boxIndex % 2 == 0) ? evenVerts : oddVerts;
                float quad[] = {
                    boxNdcLeft, boxNdcBottom, 0, 0, boxNdcRight, boxNdcBottom, 0, 0, boxNdcRight, boxNdcTop, 0, 0,
                    boxNdcLeft, boxNdcBottom, 0, 0, boxNdcRight, boxNdcTop,    0, 0, boxNdcLeft,  boxNdcTop, 0, 0,
                };
                verts.insert(verts.end(), std::begin(quad), std::end(quad));

                int displayNumber = abs(xOffset);
                float numberCenterX = boxLeft + pixelWidthOnScreen / 2.0f;
                float numberCenterY = centerY;
                CacheEyeZoomTextLabel(displayNumber, numberCenterX, numberCenterY, zoomConfig.textColor);
            }

            if (!evenVerts.empty()) {
                glUniform4f(g_solidColorShaderLocs.color, zoomConfig.gridColor1.r, zoomConfig.gridColor1.g, zoomConfig.gridColor1.b, zoomConfig.gridColor1Opacity);
                glBufferSubData(GL_ARRAY_BUFFER, 0, evenVerts.size() * sizeof(float), evenVerts.data());
                glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(evenVerts.size() / 4));
            }
            if (!oddVerts.empty()) {
                glUniform4f(g_solidColorShaderLocs.color, zoomConfig.gridColor2.r, zoomConfig.gridColor2.g, zoomConfig.gridColor2.b, zoomConfig.gridColor2Opacity);
                glBufferSubData(GL_ARRAY_BUFFER, 0, oddVerts.size() * sizeof(float), oddVerts.data());
                glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(oddVerts.size() / 4));
            }
        }

        float centerX = zoomX + zoomOutputWidth / 2.0f;
        float centerLineWidth = 2.0f;
        float lineLeft = centerX - centerLineWidth / 2.0f;
        float lineRight = centerX + centerLineWidth / 2.0f;
        float lineBottom = (float)dstBottom;
        float lineTop = (float)dstTop;

        float lineNdcLeft = (lineLeft / (float)fullW) * 2.0f - 1.0f;
        float lineNdcRight = (lineRight / (float)fullW) * 2.0f - 1.0f;
        float lineNdcBottom = (lineBottom / (float)fullH) * 2.0f - 1.0f;
        float lineNdcTop = (lineTop / (float)fullH) * 2.0f - 1.0f;

        glUniform4f(g_solidColorShaderLocs.color, zoomConfig.centerLineColor.r, zoomConfig.centerLineColor.g, zoomConfig.centerLineColor.b,
                    zoomConfig.centerLineColorOpacity);

        float centerLineVerts[] = {
            lineNdcLeft, lineNdcBottom, 0, 0, lineNdcRight, lineNdcBottom, 0, 0, lineNdcRight, lineNdcTop, 0, 0,
            lineNdcLeft, lineNdcBottom, 0, 0, lineNdcRight, lineNdcTop,    0, 0, lineNdcLeft,  lineNdcTop, 0, 0,
        };
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(centerLineVerts), centerLineVerts);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }

    glDisable(GL_BLEND);
    glBindFramebuffer(GL_FRAMEBUFFER, s.fb);
    if (oglViewport)
        oglViewport(0, 0, fullW, fullH);
    else
        glViewport(0, 0, fullW, fullH);
}

void RenderModeInternal(const ModeConfig* modeToRender, const GLState& s, int current_gameW, int current_gameH, bool skipAnimation,
                        bool excludeOnlyOnMyScreen);

void RenderMode(const ModeConfig* modeToRender, const GLState& s, int current_gameW, int current_gameH, bool skipAnimation,
                bool excludeOnlyOnMyScreen) {
    RenderModeInternal(modeToRender, s, current_gameW, current_gameH, skipAnimation, excludeOnlyOnMyScreen);
}

void RenderModeInternal(const ModeConfig* modeToRender, const GLState& s, int current_gameW, int current_gameH, bool skipAnimation,
                        bool excludeOnlyOnMyScreen) {
    PROFILE_SCOPE_CAT("RenderModeInternal", "Rendering");

    int fullW, fullH;
    {
        PROFILE_SCOPE_CAT("GetSystemMetrics", "Rendering");
        fullW = GetCachedWindowWidth();
        fullH = GetCachedWindowHeight();
    }

    // Single config snapshot for the entire frame - avoids repeated mutex acquisition
    auto configSnap = GetConfigSnapshot();

    // Get all animated state atomically to avoid race conditions
    ModeTransitionState transitionState;
    {
        PROFILE_SCOPE_CAT("GetModeTransitionState", "Rendering");
        transitionState = GetModeTransitionState();
    }
    bool transitionEffectivelyComplete = transitionState.active && transitionState.width == transitionState.targetWidth &&
                                         transitionState.height == transitionState.targetHeight &&
                                         transitionState.x == transitionState.targetX && transitionState.y == transitionState.targetY;
    bool isAnimating = transitionState.active && !skipAnimation && !transitionEffectivelyComplete;

    int modeWidth = modeToRender->width;
    int modeHeight = modeToRender->height;
    int modeX = 0;
    int modeY = 0;

    if (isAnimating) {
        modeWidth = transitionState.width;
        modeHeight = transitionState.height;
        modeX = transitionState.x;
        modeY = transitionState.y;
    }

    {
        PROFILE_SCOPE_CAT("GL State Setup", "Rendering");
        glDisable(GL_FRAMEBUFFER_SRGB);
        glDisable(GL_BLEND);
    }

    // Note: Active elements (mirrors/images/overlays) are collected on the render thread
    // via RT_CollectActiveElements. Here we only check if mirrors exist for thread startup
    bool hasMirrors = !modeToRender->mirrorIds.empty() || !modeToRender->mirrorGroupIds.empty();

    {
        PROFILE_SCOPE_CAT("Framebuffer/Viewport Setup", "Rendering");
        glBindFramebuffer(GL_FRAMEBUFFER, s.fb);
        if (oglViewport)
            oglViewport(0, 0, fullW, fullH);
        else
            glViewport(0, 0, fullW, fullH);
    }

    GLuint gameTextureToUse = g_cachedGameTextureId.load();

    GameViewportGeometry currentGeo;
    const bool useOptimizedPath =
        !isAnimating && (modeWidth == fullW && modeHeight == fullH &&
                         (!modeToRender->stretch.enabled || modeToRender->stretch.width == fullW && modeToRender->stretch.height == fullH &&
                                                                modeToRender->stretch.x == 0 && modeToRender->stretch.y == 0));

    if (useOptimizedPath) {
        PROFILE_SCOPE_CAT("Optimized Path", "Rendering");
        currentGeo = { current_gameW, current_gameH, 0, 0, fullW, fullH };

        // animations are rendered asynchronously by the render thread and never appear on the backbuffer.
    } else {
        PROFILE_SCOPE_CAT("Non-Optimized Path", "Rendering");
        int finalX, finalY, finalW, finalH;
        if (isAnimating) {
            finalX = modeX;
            finalY = modeY;
            finalW = modeWidth;
            finalH = modeHeight;
        } else if (modeToRender->stretch.enabled) {
            finalX = modeToRender->stretch.x;
            finalY = modeToRender->stretch.y;
            finalW = modeToRender->stretch.width;
            finalH = modeToRender->stretch.height;
        } else {
            finalW = modeWidth;
            finalH = modeHeight;
            finalX = (fullW - finalW) / 2;
            finalY = (fullH - finalH) / 2;
        }
        currentGeo = { current_gameW, current_gameH, finalX, finalY, finalW, finalH };
        int finalY_gl = fullH - finalY - finalH;

        int letterboxExtendX = 0;
        int letterboxExtendY = 0;
        /*if (isAnimating && transitionState.gameTransition == GameTransitionType::Bounce) {
            if (transitionState.fromWidth != transitionState.targetWidth) { letterboxExtendX = 1; }
            if (transitionState.fromHeight != transitionState.targetHeight) { letterboxExtendY = 1; }
        }*/

        glEnable(GL_SCISSOR_TEST);
        glDisable(GL_DEPTH_TEST);

        // Use fromModeId from transitionState (atomically read from snapshot) to avoid race conditions
        std::string fromModeId = transitionState.fromModeId;
        bool transitioningToFullscreen = isAnimating && EqualsIgnoreCase(modeToRender->id, "Fullscreen");
        bool transitioningFromFullscreen = isAnimating && !fromModeId.empty() && EqualsIgnoreCase(fromModeId, "Fullscreen");

        BackgroundConfig fromBackground;
        BorderConfig fromBorder;
        GLuint fromBgTex = 0;
        bool useFromBackground = false;

        if (isAnimating && !fromModeId.empty()) {
            const ModeConfig* fromMode = GetMode_Internal(fromModeId);
            if (fromMode) {
                fromBackground = fromMode->background;
                fromBorder = fromMode->border;

                bool fromHasSpecialBackground = (fromBackground.selectedMode == "gradient" || fromBackground.selectedMode == "image");
                useFromBackground = transitioningToFullscreen || fromHasSpecialBackground;
            }

            if (useFromBackground) {
                std::lock_guard<std::mutex> bgLock(g_backgroundTexturesMutex);
                auto fromBgTexIt = g_backgroundTextures.find(fromModeId);
                if (fromBgTexIt != g_backgroundTextures.end()) {
                    BackgroundTextureInstance& bgInst = fromBgTexIt->second;
                    if (bgInst.isAnimated && !bgInst.frameTextures.empty()) {
                        auto now = std::chrono::steady_clock::now();
                        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - bgInst.lastFrameTime).count();
                        int delay = bgInst.frameDelays.empty() ? 100 : bgInst.frameDelays[bgInst.currentFrame];
                        if (delay < 10) delay = 100;
                        while (elapsed >= delay) {
                            elapsed -= delay;
                            bgInst.currentFrame = (bgInst.currentFrame + 1) % bgInst.frameTextures.size();
                            delay = bgInst.frameDelays.empty() ? 100 : bgInst.frameDelays[bgInst.currentFrame];
                            if (delay < 10) delay = 100;
                        }
                        bgInst.textureId = bgInst.frameTextures[bgInst.currentFrame];
                        bgInst.lastFrameTime = now - std::chrono::milliseconds(elapsed);
                    }
                    fromBgTex = bgInst.textureId;
                }
            }
        }

        GLuint bgTex = 0;
        {
            PROFILE_SCOPE_CAT("Background Texture Lookup", "Rendering");
            std::lock_guard<std::mutex> bgLock(g_backgroundTexturesMutex);
            auto bgTexIt = g_backgroundTextures.find(modeToRender->id);
            if (bgTexIt != g_backgroundTextures.end()) {
                BackgroundTextureInstance& bgInst = bgTexIt->second;
                if (bgInst.isAnimated && !bgInst.frameTextures.empty()) {
                    auto now = std::chrono::steady_clock::now();
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - bgInst.lastFrameTime).count();
                    int delay = bgInst.frameDelays.empty() ? 100 : bgInst.frameDelays[bgInst.currentFrame];
                    if (delay < 10) delay = 100;
                    while (elapsed >= delay) {
                        elapsed -= delay;
                        bgInst.currentFrame = (bgInst.currentFrame + 1) % bgInst.frameTextures.size();
                        delay = bgInst.frameDelays.empty() ? 100 : bgInst.frameDelays[bgInst.currentFrame];
                        if (delay < 10) delay = 100;
                    }
                    bgInst.textureId = bgInst.frameTextures[bgInst.currentFrame];
                    bgInst.lastFrameTime = now - std::chrono::milliseconds(elapsed);
                }
                bgTex = bgInst.textureId;
            }
        }

        auto drawTexturedRegion = [&](int rx, int ry_gl, int rw, int rh, GLuint texId, float opacity) {
            if (rw <= 0 || rh <= 0) return;
            glScissor(rx, ry_gl, rw, rh);

            float u1 = static_cast<float>(rx) / fullW;
            float u2 = static_cast<float>(rx + rw) / fullW;
            float v1 = static_cast<float>(ry_gl) / fullH;
            float v2 = static_cast<float>(ry_gl + rh) / fullH;

            float nx1 = u1 * 2.0f - 1.0f;
            float nx2 = u2 * 2.0f - 1.0f;
            float ny1 = v1 * 2.0f - 1.0f;
            float ny2 = v2 * 2.0f - 1.0f;

            float quad[] = { nx1, ny1, u1, v1, nx2, ny1, u2, v1, nx2, ny2, u2, v2, nx1, ny1, u1, v1, nx2, ny2, u2, v2, nx1, ny2, u1, v2 };
            glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(quad), quad);
            glDrawArrays(GL_TRIANGLES, 0, 6);
        };

        auto drawColorRegion = [&](int rx, int ry_gl, int rw, int rh) {
            if (rw <= 0 || rh <= 0) return;
            glScissor(rx, ry_gl, rw, rh);

            float nx1 = (static_cast<float>(rx) / fullW) * 2.0f - 1.0f;
            float nx2 = (static_cast<float>(rx + rw) / fullW) * 2.0f - 1.0f;
            float ny1 = (static_cast<float>(ry_gl) / fullH) * 2.0f - 1.0f;
            float ny2 = (static_cast<float>(ry_gl + rh) / fullH) * 2.0f - 1.0f;

            float quad[] = { nx1, ny1, 0, 0, nx2, ny1, 0, 0, nx2, ny2, 0, 0, nx1, ny1, 0, 0, nx2, ny2, 0, 0, nx1, ny2, 0, 0 };
            glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(quad), quad);
            glDrawArrays(GL_TRIANGLES, 0, 6);
        };

        auto drawGradientRegion = [&](int rx, int ry_gl, int rw, int rh) {
            if (rw <= 0 || rh <= 0) return;
            glScissor(rx, ry_gl, rw, rh);

            float u1 = static_cast<float>(rx) / fullW;
            float u2 = static_cast<float>(rx + rw) / fullW;
            float v1 = static_cast<float>(ry_gl) / fullH;
            float v2 = static_cast<float>(ry_gl + rh) / fullH;

            float nx1 = u1 * 2.0f - 1.0f;
            float nx2 = u2 * 2.0f - 1.0f;
            float ny1 = v1 * 2.0f - 1.0f;
            float ny2 = v2 * 2.0f - 1.0f;

            float quad[] = { nx1, ny1, u1, v1, nx2, ny1, u2, v1, nx2, ny2, u2, v2, nx1, ny1, u1, v1, nx2, ny2, u2, v2, nx1, ny2, u1, v2 };
            glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(quad), quad);
            glDrawArrays(GL_TRIANGLES, 0, 6);
        };

        auto renderBackgroundImage = [&](GLuint texId, float opacity) {
            if (texId == 0) return;

            PROFILE_SCOPE_CAT("Scissor Background Image", "Rendering");

            GLint savedTexture = 0;
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &savedTexture);

            glEnable(GL_SCISSOR_TEST);
            glUseProgram(g_backgroundProgram);
            glBindTexture(GL_TEXTURE_2D, texId);
            glUniform1i(g_backgroundShaderLocs.backgroundTexture, 0);
            glUniform1f(g_backgroundShaderLocs.opacity, opacity);
            glBindVertexArray(g_vao);
            glBindBuffer(GL_ARRAY_BUFFER, g_vbo);

            if (opacity < 1.0f) {
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            } else {
                glDisable(GL_BLEND);
            }

            int vpLeft = finalX + letterboxExtendX;
            int vpRight = finalX + finalW - letterboxExtendX;
            int vpBottom_gl = finalY_gl + letterboxExtendY;
            int vpTop_gl = finalY_gl + finalH - letterboxExtendY;

            drawTexturedRegion(0, 0, fullW, vpBottom_gl, texId, opacity);
            drawTexturedRegion(0, vpTop_gl, fullW, fullH - vpTop_gl, texId, opacity);
            drawTexturedRegion(0, vpBottom_gl, vpLeft, vpTop_gl - vpBottom_gl, texId, opacity);
            drawTexturedRegion(vpRight, vpBottom_gl, fullW - vpRight, vpTop_gl - vpBottom_gl, texId, opacity);

            glDisable(GL_SCISSOR_TEST);

            glBindTexture(GL_TEXTURE_2D, savedTexture);
        };

        auto renderBackgroundColor = [&](const Color& color, float opacity) {
            PROFILE_SCOPE_CAT("Scissor Background Color", "Rendering");

            glEnable(GL_SCISSOR_TEST);
            glUseProgram(g_solidColorProgram);
            glUniform4f(g_solidColorShaderLocs.color, color.r, color.g, color.b, opacity);
            glBindVertexArray(g_vao);
            glBindBuffer(GL_ARRAY_BUFFER, g_vbo);

            if (opacity < 1.0f) {
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            } else {
                glDisable(GL_BLEND);
            }

            int vpLeft = finalX + letterboxExtendX;
            int vpRight = finalX + finalW - letterboxExtendX;
            int vpBottom_gl = finalY_gl + letterboxExtendY;
            int vpTop_gl = finalY_gl + finalH - letterboxExtendY;

            drawColorRegion(0, 0, fullW, vpBottom_gl);
            drawColorRegion(0, vpTop_gl, fullW, fullH - vpTop_gl);
            drawColorRegion(0, vpBottom_gl, vpLeft, vpTop_gl - vpBottom_gl);
            drawColorRegion(vpRight, vpBottom_gl, fullW - vpRight, vpTop_gl - vpBottom_gl);

            glDisable(GL_SCISSOR_TEST);
        };

        auto renderBackgroundGradient = [&](const BackgroundConfig& bg, float opacity) {
            if (bg.gradientStops.size() < 2) return;

            PROFILE_SCOPE_CAT("Scissor Background Gradient", "Rendering");

            glEnable(GL_SCISSOR_TEST);
            glUseProgram(g_gradientProgram);
            glBindVertexArray(g_vao);
            glBindBuffer(GL_ARRAY_BUFFER, g_vbo);

            int numStops = (std::min)(static_cast<int>(bg.gradientStops.size()), MAX_GRADIENT_STOPS);
            glUniform1i(g_gradientShaderLocs.numStops, numStops);

            float colors[MAX_GRADIENT_STOPS * 4];
            float positions[MAX_GRADIENT_STOPS];
            for (int i = 0; i < numStops; i++) {
                colors[i * 4 + 0] = bg.gradientStops[i].color.r;
                colors[i * 4 + 1] = bg.gradientStops[i].color.g;
                colors[i * 4 + 2] = bg.gradientStops[i].color.b;
                colors[i * 4 + 3] = opacity;
                positions[i] = bg.gradientStops[i].position;
            }
            glUniform4fv(g_gradientShaderLocs.stopColors, numStops, colors);
            glUniform1fv(g_gradientShaderLocs.stopPositions, numStops, positions);
            glUniform1f(g_gradientShaderLocs.angle, bg.gradientAngle * 3.14159265f / 180.0f);

            static auto startTime = std::chrono::steady_clock::now();
            auto now = std::chrono::steady_clock::now();
            float timeSeconds = std::chrono::duration<float>(now - startTime).count();
            glUniform1f(g_gradientShaderLocs.time, timeSeconds);
            glUniform1i(g_gradientShaderLocs.animationType, static_cast<int>(bg.gradientAnimation));
            glUniform1f(g_gradientShaderLocs.animationSpeed, bg.gradientAnimationSpeed);
            glUniform1i(g_gradientShaderLocs.colorFade, bg.gradientColorFade ? 1 : 0);

            if (opacity < 1.0f) {
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            } else {
                glDisable(GL_BLEND);
            }

            int vpLeft = finalX + letterboxExtendX;
            int vpRight = finalX + finalW - letterboxExtendX;
            int vpBottom_gl = finalY_gl + letterboxExtendY;
            int vpTop_gl = finalY_gl + finalH - letterboxExtendY;

            drawGradientRegion(0, 0, fullW, vpBottom_gl);
            drawGradientRegion(0, vpTop_gl, fullW, fullH - vpTop_gl);
            drawGradientRegion(0, vpBottom_gl, vpLeft, vpTop_gl - vpBottom_gl);
            drawGradientRegion(vpRight, vpBottom_gl, fullW - vpRight, vpTop_gl - vpBottom_gl);

            glDisable(GL_SCISSOR_TEST);
        };

        if (useFromBackground) {
            PROFILE_SCOPE_CAT("Render From Background", "Rendering");
            if (fromBackground.selectedMode == "image" && fromBgTex != 0) {
                renderBackgroundImage(fromBgTex, 1.0f);
            } else if (fromBackground.selectedMode == "gradient" && fromBackground.gradientStops.size() >= 2) {
                renderBackgroundGradient(fromBackground, 1.0f);
            } else {
                renderBackgroundColor(fromBackground.color, 1.0f);
            }
        }

        if (!useFromBackground) {
            PROFILE_SCOPE_CAT("Render To Background", "Rendering");
            if (modeToRender->background.selectedMode == "image" && bgTex != 0) {
                renderBackgroundImage(bgTex, 1.0f);
            } else if (modeToRender->background.selectedMode == "gradient" && modeToRender->background.gradientStops.size() >= 2) {
                renderBackgroundGradient(modeToRender->background, 1.0f);
            } else {
                renderBackgroundColor(modeToRender->background.color, 1.0f);
            }
        }

        glDisable(GL_SCISSOR_TEST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g_sceneFBO);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s.fb);

        {
            PROFILE_SCOPE_CAT("Render Game Border", "Rendering");
            if (transitioningToFullscreen && fromBorder.enabled && fromBorder.width > 0) {
                RenderGameBorder(finalX, finalY, finalW, finalH, fromBorder.width, fromBorder.radius, fromBorder.color, fullW, fullH);
            } else if (modeToRender->border.enabled && modeToRender->border.width > 0) {
                RenderGameBorder(finalX, finalY, finalW, finalH, modeToRender->border.width, modeToRender->border.radius,
                                 modeToRender->border.color, fullW, fullH);
            }
        }
    }

    bool useFramebufferFallback = (gameTextureToUse == UINT_MAX);

    static bool fallbackLogged = false;
    if (useFramebufferFallback && !fallbackLogged) {
        Log("Mirror rendering using framebuffer fallback mode (glClear hook disabled for this game version)");
        fallbackLogged = true;
    } else if (!useFramebufferFallback && fallbackLogged) {
        Log("Mirror rendering switched to standard texture mode (glClear hook active)");
        fallbackLogged = false;
    }

    {
        PROFILE_SCOPE_CAT("Set Viewport Geometry", "Rendering");
        std::lock_guard<std::mutex> lock(g_geometryMutex);
        g_lastFrameGeometry = currentGeo;
    }

    // Start capture/render threads and update game state for mirror capture
    if (hasMirrors) {
        PROFILE_SCOPE_CAT("Mirror Thread Management", "Rendering");

        // Update game state for capture thread
        g_captureGameTexture.store(gameTextureToUse);
        g_captureGameW.store(current_gameW);
        g_captureGameH.store(current_gameH);

        // Lazy auto-start capture thread when game texture is available
        if (!useFramebufferFallback && !g_mirrorCaptureRunning.load()) {
            HGLRC gameContext = wglGetCurrentContext();
            if (gameContext) {
                // Capture texture init is now done inside StartMirrorCaptureThread after wglShareLists
                StartMirrorCaptureThread(gameContext);
            }
        }

        if (!useFramebufferFallback && g_graphicsHookDetected.load()) { StartObsHookThread(); }

        // Auto-start render thread for async overlay compositing
        if (!g_renderThreadRunning.load()) {
            HGLRC gameContext = wglGetCurrentContext();
            if (gameContext) { StartRenderThread(gameContext); }
        }

        // NOTE: Mirror capture config updates are now handled by logic_thread (UpdateActiveMirrorConfigs)
        // This avoids doing the config collection work on every frame of the main render thread

        // FRAMEBUFFER FALLBACK MODE: Capture directly on main thread when game texture unavailable
        if (useFramebufferFallback) {
            auto now = std::chrono::steady_clock::now();

            // Collect active mirrors for fallback rendering (use snapshot for thread safety)
            std::vector<MirrorConfig> fallbackMirrors;
            const auto& fbMirrors = configSnap ? configSnap->mirrors : g_config.mirrors;
            const auto& fbGroups = configSnap ? configSnap->mirrorGroups : g_config.mirrorGroups;
            fallbackMirrors.reserve(modeToRender->mirrorIds.size() + modeToRender->mirrorGroupIds.size());

            std::unordered_map<std::string, size_t> mirrorIndex;
            mirrorIndex.reserve(fbMirrors.size());
            for (size_t i = 0; i < fbMirrors.size(); ++i) { mirrorIndex[fbMirrors[i].name] = i; }
            std::unordered_map<std::string, size_t> groupIndex;
            groupIndex.reserve(fbGroups.size());
            for (size_t i = 0; i < fbGroups.size(); ++i) { groupIndex[fbGroups[i].name] = i; }

            for (const auto& name : modeToRender->mirrorIds) {
                auto it = mirrorIndex.find(name);
                if (it != mirrorIndex.end()) {
                    fallbackMirrors.push_back(fbMirrors[it->second]);
                }
            }
            for (const auto& groupName : modeToRender->mirrorGroupIds) {
                auto git = groupIndex.find(groupName);
                if (git != groupIndex.end()) {
                    const auto& group = fbGroups[git->second];
                    for (const auto& item : group.mirrors) {
                        if (!item.enabled) continue;
                        auto mit = mirrorIndex.find(item.mirrorId);
                        if (mit != mirrorIndex.end()) {
                            const auto& mirror = fbMirrors[mit->second];
                            MirrorConfig groupedMirror = mirror;
                            int groupX = group.output.x;
                            int groupY = group.output.y;
                            if (group.output.useRelativePosition) {
                                int screenW = GetCachedWindowWidth();
                                int screenH = GetCachedWindowHeight();
                                groupX = static_cast<int>(group.output.relativeX * screenW);
                                groupY = static_cast<int>(group.output.relativeY * screenH);
                            }
                            groupedMirror.output.x = groupX + item.offsetX;
                            groupedMirror.output.y = groupY + item.offsetY;
                            groupedMirror.output.relativeTo = group.output.relativeTo;
                            groupedMirror.output.useRelativePosition = group.output.useRelativePosition;
                            groupedMirror.output.relativeX = group.output.relativeX;
                            groupedMirror.output.relativeY = group.output.relativeY;
                            if (item.widthPercent != 1.0f || item.heightPercent != 1.0f) {
                                groupedMirror.output.separateScale = true;
                                float baseScaleX = mirror.output.separateScale ? mirror.output.scaleX : mirror.output.scale;
                                float baseScaleY = mirror.output.separateScale ? mirror.output.scaleY : mirror.output.scale;
                                groupedMirror.output.scaleX = baseScaleX * item.widthPercent;
                                groupedMirror.output.scaleY = baseScaleY * item.heightPercent;
                            }
                            fallbackMirrors.push_back(groupedMirror);
                        }
                    }
                }
            }

            std::vector<size_t> mirrorsNeedingUpdate;
            mirrorsNeedingUpdate.reserve(fallbackMirrors.size());

            {
                std::shared_lock<std::shared_mutex> mirrorLock(g_mirrorInstancesMutex); // Read lock - checking which mirrors need update
                for (size_t i = 0; i < fallbackMirrors.size(); ++i) {
                    const auto& conf = fallbackMirrors[i];
                    if (conf.input.empty() || conf.captureWidth <= 0 || conf.captureHeight <= 0) continue;

                    auto it = g_mirrorInstances.find(conf.name);
                    if (it == g_mirrorInstances.end()) continue;

                    const MirrorInstance& inst = it->second;

                    int padding = (conf.border.type == MirrorBorderType::Dynamic) ? conf.border.dynamicThickness : 0;
                    int requiredFboW = conf.captureWidth + 2 * padding;
                    int requiredFboH = conf.captureHeight + 2 * padding;
                    bool needsResize = (inst.fbo_w != requiredFboW || inst.fbo_h != requiredFboH);

                    bool needsUpdate = needsResize || inst.forceUpdateFrames > 0;
                    if (!needsUpdate && conf.fps > 0) {
                        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - inst.lastUpdateTime).count();
                        needsUpdate = (elapsed >= (1000 / conf.fps));
                    } else if (!needsUpdate && conf.fps <= 0) {
                        needsUpdate = true;
                    }

                    if (needsUpdate) { mirrorsNeedingUpdate.push_back(i); }
                }
            }

            if (!mirrorsNeedingUpdate.empty()) {
                glBindVertexArray(g_vao);
                glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
                glEnable(GL_BLEND);
                glBlendFunc(GL_ONE, GL_ONE);

                PROFILE_SCOPE_CAT("Fallback Mirror Lock", "Rendering");
                std::unique_lock<std::shared_mutex> mirrorLock(g_mirrorInstancesMutex); // Write lock - modifying instances

                for (size_t idx : mirrorsNeedingUpdate) {
                    const auto& conf = fallbackMirrors[idx];

                    auto it = g_mirrorInstances.find(conf.name);
                    if (it == g_mirrorInstances.end()) continue;

                    MirrorInstance& inst = it->second;
                    int padding = (conf.border.type == MirrorBorderType::Dynamic) ? conf.border.dynamicThickness : 0;
                    int requiredFboW = conf.captureWidth + 2 * padding;
                    int requiredFboH = conf.captureHeight + 2 * padding;

                    if (inst.fbo_w != requiredFboW || inst.fbo_h != requiredFboH) {
                        inst.fbo_w = requiredFboW;
                        inst.fbo_h = requiredFboH;
                        inst.forceUpdateFrames = 3;

                        glBindTexture(GL_TEXTURE_2D, inst.fboTexture);
                        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, inst.fbo_w, inst.fbo_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

                        glBindTexture(GL_TEXTURE_2D, inst.fboTextureBack);
                        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, inst.fbo_w, inst.fbo_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                    }

                    glBindFramebuffer(GL_FRAMEBUFFER, inst.fbo);
                    if (oglViewport)
                        oglViewport(0, 0, inst.fbo_w, inst.fbo_h);
                    else
                        glViewport(0, 0, inst.fbo_w, inst.fbo_h);
                    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
                    glClear(GL_COLOR_BUFFER_BIT);

                    glBindFramebuffer(GL_READ_FRAMEBUFFER, s.fb);
                    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, inst.fbo);

                    for (const auto& r : conf.input) {
                        int capX, capY;
                        GetRelativeCoords(r.relativeTo, r.x, r.y, conf.captureWidth, conf.captureHeight, current_gameW, current_gameH, capX,
                                          capY);
                        int capY_gl = current_gameH - capY - conf.captureHeight;

                        float scaleX = static_cast<float>(currentGeo.finalW) / current_gameW;
                        float scaleY = static_cast<float>(currentGeo.finalH) / current_gameH;

                        int srcLeft = currentGeo.finalX + static_cast<int>(capX * scaleX);
                        int srcBottom = fullH - currentGeo.finalY - static_cast<int>((capY + conf.captureHeight) * scaleY);
                        int srcRight = currentGeo.finalX + static_cast<int>((capX + conf.captureWidth) * scaleX);
                        int srcTop = fullH - currentGeo.finalY - static_cast<int>(capY * scaleY);

                        int dstLeft = padding;
                        int dstBottom = padding;
                        int dstRight = padding + conf.captureWidth;
                        int dstTop = padding + conf.captureHeight;

                        glBlitFramebuffer(srcLeft, srcBottom, srcRight, srcTop, dstLeft, dstBottom, dstRight, dstTop, GL_COLOR_BUFFER_BIT,
                                          GL_NEAREST);
                    }

                    glBindFramebuffer(GL_FRAMEBUFFER, inst.fbo);
                    inst.lastUpdateTime = now;
                    inst.hasValidContent = true;
                    inst.capturedAsRawOutput = true;
                    if (inst.forceUpdateFrames > 0) { inst.forceUpdateFrames--; }
                }

                glDisable(GL_BLEND);
            }
        }
    }

    glBindFramebuffer(GL_FRAMEBUFFER, s.fb);
    if (oglViewport)
        oglViewport(0, 0, fullW, fullH);
    else
        glViewport(0, 0, fullW, fullH);

    if (g_imageDragMode.load() && g_imageOverlaysVisible.load(std::memory_order_acquire)) {
        PROFILE_SCOPE_CAT("Image Drag Mode", "Input Handling");
        ImGuiIO& io = ImGui::GetIO();
        if (io.WantCaptureMouse) {
            s_hoveredImageName = "";
        } else {
            HWND hwnd = g_minecraftHwnd.load();
            if (hwnd) {
                POINT mousePos;
                GetCursorPos(&mousePos);
                ScreenToClient(hwnd, &mousePos);

                if (mousePos.x >= s.vp[0] && mousePos.x < (s.vp[0] + s.vp[2]) && mousePos.y >= s.vp[1] &&
                    mousePos.y < (s.vp[1] + s.vp[3])) {

                    bool leftButtonDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;

                    std::string hoveredImage = "";
                    const auto& dragImages = configSnap ? configSnap->images : std::vector<ImageConfig>{};

                    // Drag mode is rare, but this is on the game thread, so keep it cheap.
                    std::unordered_map<std::string, const ImageConfig*> imageByName;
                    if (configSnap) {
                        imageByName.reserve(dragImages.size());
                        for (const auto& img : dragImages) {
                            imageByName.emplace(img.name, &img);
                        }
                    }
                    for (const auto& imageName : modeToRender->imageIds) {
                        const ImageConfig* confPtr = nullptr;
                        if (!imageByName.empty()) {
                            auto it = imageByName.find(imageName);
                            if (it != imageByName.end()) { confPtr = it->second; }
                        } else {
                            for (const auto& img : dragImages) {
                                if (img.name == imageName) {
                                    confPtr = &img;
                                    break;
                                }
                            }
                        }
                        if (!confPtr) continue;
                        const ImageConfig& conf = *confPtr;

                        // Try-lock to avoid stalling the game thread if textures are being updated.
                        int texWidth = 0;
                        int texHeight = 0;
                        {
                            std::unique_lock<std::mutex> imageLock(g_userImagesMutex, std::try_to_lock);
                            if (!imageLock.owns_lock()) { continue; }
                            auto it_inst = g_userImages.find(conf.name);
                            if (it_inst == g_userImages.end() || it_inst->second.textureId == 0) continue;
                            texWidth = it_inst->second.width;
                            texHeight = it_inst->second.height;
                        }

                        // Calculate actual dimensions from scale (avoid calling CalculateImageDimensions to prevent nested locking)
                        int croppedW = texWidth - conf.crop_left - conf.crop_right;
                        int croppedH = texHeight - conf.crop_top - conf.crop_bottom;
                        if (croppedW < 1) croppedW = 1;
                        if (croppedH < 1) croppedH = 1;
                        int displayW = static_cast<int>(croppedW * conf.scale);
                        int displayH = static_cast<int>(croppedH * conf.scale);
                        if (displayW < 1) displayW = 1;
                        if (displayH < 1) displayH = 1;

                        int finalScreenX_win, finalScreenY_win;
                        GetRelativeCoordsForImageWithViewport(conf.relativeTo, conf.x, conf.y, displayW, displayH, currentGeo.finalX,
                                                              currentGeo.finalY, currentGeo.finalW, currentGeo.finalH, fullW, fullH,
                                                              finalScreenX_win, finalScreenY_win);

                        if (mousePos.x >= finalScreenX_win && mousePos.x < finalScreenX_win + displayW && mousePos.y >= finalScreenY_win &&
                            mousePos.y < finalScreenY_win + displayH) {
                            hoveredImage = conf.name;
                            break;
                        }
                    }

                    if (leftButtonDown && !s_isDragging && !hoveredImage.empty()) {
                        s_isDragging = true;
                        s_draggedImageName = hoveredImage;
                        s_dragStartPos = mousePos;
                        s_lastMousePos = mousePos;
                    }
                    else if (leftButtonDown && s_isDragging && !s_draggedImageName.empty()) {
                        int deltaX = mousePos.x - s_lastMousePos.x;
                        int deltaY = mousePos.y - s_lastMousePos.y;

                        if (deltaX != 0 || deltaY != 0) {
                            // NOTE: This mutates g_config from the game thread. Safe because:
                            // 1. Only this thread writes drag x/y (no concurrent x/y writers)
                            // 2. GUI thread won't resize images vector during drag mode (drag mode disables GUI interaction)
                            {
                                for (auto& img : g_config.images) {
                                    if (img.name == s_draggedImageName) {
                                        img.x += deltaX;
                                        img.y += deltaY;
                                        g_configIsDirty = true;
                                        break;
                                    }
                                }
                            }

                            s_lastMousePos = mousePos;
                        }
                    }
                    else if (!leftButtonDown && s_isDragging) {
                        s_isDragging = false;
                        s_draggedImageName = "";
                    }

                    s_hoveredImageName = hoveredImage;
                }
            }
        }
    } else {
        if (s_isDragging) {
            s_isDragging = false;
            s_draggedImageName = "";
            s_hoveredImageName = "";
        }
    }

    if (g_showGui.load(std::memory_order_relaxed) && g_windowOverlayDragMode.load(std::memory_order_relaxed) &&
        g_windowOverlaysVisible.load(std::memory_order_acquire)) {
        PROFILE_SCOPE_CAT("Window Overlay Drag Mode", "Input Handling");

        ImGuiIO& io = ImGui::GetIO();
        if (io.WantCaptureMouse) {
            s_hoveredWindowOverlayName = "";
        } else {
            HWND hwnd = g_minecraftHwnd.load();
            if (hwnd) {
                POINT mousePos;
                GetCursorPos(&mousePos);
                ScreenToClient(hwnd, &mousePos);

                if (mousePos.x >= s.vp[0] && mousePos.x < (s.vp[0] + s.vp[2]) && mousePos.y >= s.vp[1] &&
                    mousePos.y < (s.vp[1] + s.vp[3])) {

                    bool leftButtonDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;

                    // Keep previous hover state if we can't get the mutexes this frame
                    std::string hoveredOverlay = s_hoveredWindowOverlayName;

                    if (!s_isWindowOverlayDragging) {
                        PROFILE_SCOPE_CAT("Overlay Hover Detection", "Input Handling");

                        // Try to acquire cache mutex - skip hover detection if busy
                        std::unique_lock<std::mutex> cacheLock(g_windowOverlayCacheMutex, std::try_to_lock);

                        if (cacheLock.owns_lock()) {
                            hoveredOverlay = "";
                            std::vector<const WindowOverlayConfig*> activeOverlays;

                            std::unordered_map<std::string, const WindowOverlayConfig*> overlayByName;
                            if (configSnap) {
                                overlayByName.reserve(configSnap->windowOverlays.size());
                                for (const auto& ov : configSnap->windowOverlays) {
                                    overlayByName.emplace(ov.name, &ov);
                                }
                            }
                            for (const auto& overlayId : modeToRender->windowOverlayIds) {
                                const WindowOverlayConfig* config = nullptr;
                                if (!overlayByName.empty()) {
                                    auto it = overlayByName.find(overlayId);
                                    if (it != overlayByName.end()) { config = it->second; }
                                } else {
                                    config = configSnap ? FindWindowOverlayConfigIn(overlayId, *configSnap) : nullptr;
                                }
                                if (config) { activeOverlays.push_back(config); }
                            }

                            for (const WindowOverlayConfig* confPtr : activeOverlays) {
                                if (!confPtr) continue;
                                const WindowOverlayConfig& conf = *confPtr;
                                // Use Unsafe version since we already hold the cache mutex
                                int displayW, displayH;
                                CalculateWindowOverlayDimensionsUnsafe(conf, displayW, displayH);
                                int finalScreenX_win, finalScreenY_win;
                                GetRelativeCoordsForImageWithViewport(conf.relativeTo, conf.x, conf.y, displayW, displayH,
                                                                      currentGeo.finalX, currentGeo.finalY, currentGeo.finalW,
                                                                      currentGeo.finalH, fullW, fullH, finalScreenX_win, finalScreenY_win);

                                if (mousePos.x >= finalScreenX_win && mousePos.x < finalScreenX_win + displayW &&
                                    mousePos.y >= finalScreenY_win && mousePos.y < finalScreenY_win + displayH) {
                                    hoveredOverlay = conf.name;
                                    break;
                                }
                            }
                        }
                        // If we couldn't get both locks, hover detection is skipped this frame
                    }

                    if (leftButtonDown && !s_isWindowOverlayDragging && !hoveredOverlay.empty()) {
                        s_isWindowOverlayDragging = true;
                        s_draggedWindowOverlayName = hoveredOverlay;
                        s_lastMousePos = mousePos;

                        // Read initial position from snapshot for thread safety
                        if (configSnap) {
                            for (const auto& overlay : configSnap->windowOverlays) {
                                if (overlay.name == s_draggedWindowOverlayName) {
                                    s_initialX = overlay.x;
                                    s_initialY = overlay.y;
                                    break;
                                }
                            }
                        }
                    }
                    else if (leftButtonDown && s_isWindowOverlayDragging && !s_draggedWindowOverlayName.empty()) {
                        PROFILE_SCOPE_CAT("Overlay Drag Update", "Input Handling");

                        int deltaX = mousePos.x - s_lastMousePos.x;
                        int deltaY = mousePos.y - s_lastMousePos.y;

                        if (deltaX != 0 || deltaY != 0) {
                            // Mutate g_config from game thread - safe because:
                            // 1. Only this thread writes drag x/y (no concurrent x/y writers)
                            for (auto& overlay : g_config.windowOverlays) {
                                if (overlay.name == s_draggedWindowOverlayName) {
                                    overlay.x += deltaX;
                                    overlay.y += deltaY;
                                    g_configIsDirty = true;
                                    break;
                                }
                            }

                            s_lastMousePos = mousePos;
                        }
                    }
                    else if (!leftButtonDown && s_isWindowOverlayDragging) {
                        s_isWindowOverlayDragging = false;
                        s_draggedWindowOverlayName = "";
                        SaveConfigImmediate();
                    }

                    s_hoveredWindowOverlayName = hoveredOverlay;
                }
            }
        }
    } else {
        if (s_isWindowOverlayDragging) {
            s_isWindowOverlayDragging = false;
            s_draggedWindowOverlayName = "";
            s_hoveredWindowOverlayName = "";
        }
    }

    float overlayOpacity = 1.0f;

    // If there's nothing to draw, avoid waking the render thread and avoid compositing a fullscreen overlay texture.
    const bool wantOverlayElements = hasMirrors ||
                                    (g_imageOverlaysVisible.load(std::memory_order_acquire) && !modeToRender->imageIds.empty()) ||
                                    (g_windowOverlaysVisible.load(std::memory_order_acquire) && !modeToRender->windowOverlayIds.empty());
    const bool wantAnyImGui = g_shouldRenderGui.load(std::memory_order_relaxed) || g_showPerformanceOverlay.load(std::memory_order_relaxed) ||
                              g_showProfiler.load(std::memory_order_relaxed) || g_showEyeZoom.load(std::memory_order_relaxed) ||
                              g_showTextureGrid.load(std::memory_order_relaxed);
    const bool isFullscreenMode = EqualsIgnoreCase(modeToRender->id, "Fullscreen");

    bool wantWelcomeToast = false;
    {
        static bool s_prevFullscreen = false;
        static std::chrono::steady_clock::time_point s_fullscreenEnterTime{};
        auto now = std::chrono::steady_clock::now();
        if (isFullscreenMode && !s_prevFullscreen) { s_fullscreenEnterTime = now; }
        s_prevFullscreen = isFullscreenMode;

        if (isFullscreenMode && !g_configurePromptDismissedThisSession.load(std::memory_order_relaxed)) {
            constexpr float kHoldSeconds = 10.0f;
            constexpr float kFadeSeconds = 1.5f;
            const float elapsed = std::chrono::duration_cast<std::chrono::duration<float>>(now - s_fullscreenEnterTime).count();
            wantWelcomeToast = (elapsed <= (kHoldSeconds + kFadeSeconds + 0.25f));
        }
    }

    const bool wantAsyncOverlayThisFrame = wantOverlayElements || wantAnyImGui || wantWelcomeToast;

    // All overlay rendering is done on the render thread
    // Submit lightweight request - render thread looks up active elements from g_config
    // This moves collection + rendering work off the main thread, reducing SwapBuffers hook time

    if (g_renderThreadRunning.load() && wantAsyncOverlayThisFrame) {
        PROFILE_SCOPE_CAT("Async Overlay Submit/Blit", "Rendering");

        {
            PROFILE_SCOPE_CAT("Submit Frame For Rendering", "Rendering");
            // Submit current frame's data to render thread (non-blocking)
            // Render thread will look up active mirrors/images/overlays from g_config
            static uint64_t s_frameNumber = 0;
            s_frameNumber++;

            FrameRenderRequest request;
            request.frameNumber = s_frameNumber;
            request.fullW = fullW;
            request.fullH = fullH;
            request.gameW = current_gameW;
            request.gameH = current_gameH;
            request.finalX = currentGeo.finalX;
            request.finalY = currentGeo.finalY;
            request.finalW = currentGeo.finalW;
            request.finalH = currentGeo.finalH;
            request.gameTextureId = gameTextureToUse;
            request.modeId = modeToRender->id;
            request.isAnimating = isAnimating;
            request.overlayOpacity = overlayOpacity;
            request.obsDetected = g_graphicsHookDetected.load();
            request.excludeOnlyOnMyScreen = excludeOnlyOnMyScreen;
            request.skipAnimation = skipAnimation;
            request.relativeStretching = modeToRender->relativeStretching;

            bool transitionEffectivelyCompleteForOverlays = transitionState.active && transitionState.moveProgress >= 1.0f;
            bool overlaysShouldLerp = transitionState.active && !transitionEffectivelyCompleteForOverlays &&
                                      transitionState.overlayTransition != OverlayTransitionType::Cut;
            if (overlaysShouldLerp) {
                request.transitionProgress = transitionState.moveProgress;
                request.fromW = transitionState.fromWidth;
                request.fromH = transitionState.fromHeight;
                request.fromX = transitionState.fromX;
                request.fromY = transitionState.fromY;
                request.toW = transitionState.targetWidth;
                request.toH = transitionState.targetHeight;
                request.toX = transitionState.targetX;
                request.toY = transitionState.targetY;
            } else {
                request.transitionProgress = 1.0f;
                request.fromX = currentGeo.finalX;
                request.fromY = currentGeo.finalY;
                request.fromW = currentGeo.finalW;
                request.fromH = currentGeo.finalH;
                request.toX = currentGeo.finalX;
                request.toY = currentGeo.finalY;
                request.toW = currentGeo.finalW;
                request.toH = currentGeo.finalH;
            }

            // Populate ImGui rendering state from global atomics
            // Acquire isTransitioningFromEyeZoom first - it's the release-paired store,
            request.isTransitioningFromEyeZoom = g_isTransitioningFromEyeZoom.load(std::memory_order_acquire);
            request.shouldRenderGui = g_shouldRenderGui.load(std::memory_order_relaxed);
            request.showPerformanceOverlay = g_showPerformanceOverlay.load(std::memory_order_relaxed);
            request.showProfiler = g_showProfiler.load(std::memory_order_relaxed);
            request.showEyeZoom = g_showEyeZoom.load(std::memory_order_relaxed);
            request.eyeZoomFadeOpacity = g_eyeZoomFadeOpacity.load(std::memory_order_relaxed);
            request.eyeZoomAnimatedViewportX = skipAnimation ? -1 : g_eyeZoomAnimatedViewportX.load(std::memory_order_relaxed);
            request.eyeZoomSnapshotTexture = GetEyeZoomSnapshotTexture();
            request.eyeZoomSnapshotWidth = GetEyeZoomSnapshotWidth();
            request.eyeZoomSnapshotHeight = GetEyeZoomSnapshotHeight();
            request.showTextureGrid = g_showTextureGrid.load(std::memory_order_relaxed);
            request.textureGridModeWidth = g_textureGridModeWidth.load(std::memory_order_relaxed);
            request.textureGridModeHeight = g_textureGridModeHeight.load(std::memory_order_relaxed);

            request.welcomeToastIsFullscreen = isFullscreenMode;
            request.showWelcomeToast = wantWelcomeToast;

            request.backgroundIsImage = (modeToRender->background.selectedMode == "image");
            request.bgR = modeToRender->background.color.r;
            request.bgG = modeToRender->background.color.g;
            request.bgB = modeToRender->background.color.b;
            request.borderEnabled = modeToRender->border.enabled;
            request.borderR = modeToRender->border.color.r;
            request.borderG = modeToRender->border.color.g;
            request.borderB = modeToRender->border.color.b;
            request.borderWidth = modeToRender->border.width;
            request.borderRadius = modeToRender->border.radius;

            request.transitioningToFullscreen = isAnimating && EqualsIgnoreCase(modeToRender->id, "Fullscreen");
            request.fromModeId = transitionState.fromModeId;
            if (!transitionState.fromModeId.empty()) {
                const ModeConfig* fromMode = GetMode_Internal(transitionState.fromModeId);
                if (fromMode) {
                    request.fromSlideMirrorsIn = fromMode->slideMirrorsIn;
                    if (request.transitioningToFullscreen) {
                        request.fromBackgroundIsImage = (fromMode->background.selectedMode == "image");
                        request.fromBgR = fromMode->background.color.r;
                        request.fromBgG = fromMode->background.color.g;
                        request.fromBgB = fromMode->background.color.b;
                        request.fromBorderEnabled = fromMode->border.enabled;
                        request.fromBorderR = fromMode->border.color.r;
                        request.fromBorderG = fromMode->border.color.g;
                        request.fromBorderB = fromMode->border.color.b;
                        request.fromBorderWidth = fromMode->border.width;
                        request.fromBorderRadius = fromMode->border.radius;
                    }
                }
            }
            request.toSlideMirrorsIn = modeToRender->slideMirrorsIn;

            if (transitionState.active && transitionState.moveProgress < 1.0f) {
                request.mirrorSlideProgress = transitionState.moveProgress;
            } else {
                request.mirrorSlideProgress = 1.0f;
            }

            if (isAnimating && transitionState.gameTransition == GameTransitionType::Bounce) {
                if (transitionState.fromWidth != transitionState.targetWidth) { request.letterboxExtendX = 1; }
                if (transitionState.fromHeight != transitionState.targetHeight) { request.letterboxExtendY = 1; }
            }

            SubmitFrameForRendering(request);
        }

        // Note: EyeZoom rendering is now done entirely on the render thread via RT_RenderEyeZoom
        // using the synchronized ready frame from mirror thread for flicker-free capture

        // This introduces 1 frame of latency for overlays but keeps the main thread fast
        CompletedRenderFrame completed = GetCompletedRenderFrame();
        GLuint completedTexture = completed.texture;
        if (completedTexture != 0) {
            PROFILE_SCOPE_CAT("Blit Async Overlay Result", "Rendering");

            // Wait on the render thread's fence to ensure texture is fully rendered
            // glWaitSync is a GPU-side wait that doesn't block the CPU like glFinish
            GLsync fence = completed.fence;
            // NOTE: Under very high FPS / scheduler jitter, a fence can be rotated out and deleted
            // by the render thread before we reach glWaitSync (TOCTOU). glIsSync guards against
            if (fence && glIsSync(fence)) { glWaitSync(fence, 0, GL_TIMEOUT_IGNORED); }

            // Memory barrier to ensure we see the latest texture data from render thread
            glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_FRAMEBUFFER_BARRIER_BIT);

            glBindVertexArray(g_fullscreenQuadVAO);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, completedTexture);

            glUseProgram(g_backgroundProgram);
            glUniform1f(g_backgroundShaderLocs.opacity, 1.0f);

            // The render_thread output is NOT premultiplied (ImGui OpenGL3 backend + our shaders output straight alpha).
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

            glDrawArrays(GL_TRIANGLES, 0, 6);

            glDisable(GL_BLEND);

            // Publish a consumer fence for this specific completed FBO.
            // This prevents the render thread from reusing/clearing the same texture while the GPU
            if (completed.fboIndex >= 0) {
                GLsync consumerFence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
                SubmitRenderFBOConsumerFence(completed.fboIndex, consumerFence);
            }

            // Create fence after blit for delayRenderingUntilBlitted setting
            // Delete any existing fence first (shouldn't happen normally, but be safe)
            GLsync oldFence = g_overlayBlitFence.exchange(nullptr, std::memory_order_acq_rel);
            if (oldFence && glIsSync(oldFence)) { glDeleteSync(oldFence); }
            g_overlayBlitFence.store(glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0), std::memory_order_release);
        }
    }

    if (g_showGui && !g_currentlyEditingMirror.empty()) {
        PROFILE_SCOPE_CAT("Debug Borders", "Rendering");
        if (MirrorConfig* conf = GetMutableMirror(g_currentlyEditingMirror)) {
            RenderDebugBordersForMirror(conf, { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, s.va);
        }
    }
}
void RenderDebugBordersForMirror(const MirrorConfig* conf, Color captureColor, Color outputColor, GLint originalVAO) {
    if (!conf || !g_glInitialized.load(std::memory_order_acquire)) return;

    const int fullW = GetCachedWindowWidth();
    const int fullH = GetCachedWindowHeight();

    GameViewportGeometry geo;
    {
        std::lock_guard<std::mutex> lock(g_geometryMutex);
        geo = g_lastFrameGeometry;
    }

    glUseProgram(g_solidColorProgram);
    glLineWidth(2.0f);
    glDisable(GL_BLEND);

    glBindVertexArray(g_debugVAO);
    glBindBuffer(GL_ARRAY_BUFFER, g_debugVBO);

    float xScale = geo.gameW > 0 ? (float)geo.finalW / geo.gameW : 1.0f;
    float yScale = geo.gameH > 0 ? (float)geo.finalH / geo.gameH : 1.0f;

    glUniform4f(g_solidColorShaderLocs.color, captureColor.r, captureColor.g, captureColor.b, 1.0f);
    for (const auto& r : conf->input) {
        int capX, capY;
        GetRelativeCoords(r.relativeTo, r.x, r.y, conf->captureWidth, conf->captureHeight, geo.gameW, geo.gameH, capX, capY);

        int scaled_capX = geo.finalX + static_cast<int>(capX * xScale);
        int scaled_capY = geo.finalY + static_cast<int>(capY * yScale);
        int scaled_capW = static_cast<int>(conf->captureWidth * xScale);
        int scaled_capH = static_cast<int>(conf->captureHeight * yScale);

        int scaled_capY_gl = fullH - scaled_capY - scaled_capH;

        float x1 = (static_cast<float>(scaled_capX) / fullW) * 2.0f - 1.0f;
        float y1 = (static_cast<float>(scaled_capY_gl) / fullH) * 2.0f - 1.0f;
        float x2 = (static_cast<float>(scaled_capX + scaled_capW) / fullW) * 2.0f - 1.0f;
        float y2 = (static_cast<float>(scaled_capY_gl + scaled_capH) / fullH) * 2.0f - 1.0f;
        float v[] = { x1, y1, x2, y1, x2, y2, x1, y2 };
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(v), v);
        glDrawArrays(GL_LINE_LOOP, 0, 4);
    }

    auto instIt = g_mirrorInstances.find(conf->name);
    if (instIt != g_mirrorInstances.end()) {
        const MirrorInstance& inst = instIt->second;
        int finalScreenX, finalScreenY_win;
        CalculateFinalScreenPos(conf, inst, geo.gameW, geo.gameH, geo.finalX, geo.finalY, geo.finalW, geo.finalH, fullW, fullH,
                                finalScreenX, finalScreenY_win);
        float scaleX = conf->output.separateScale ? conf->output.scaleX : conf->output.scale;
        float scaleY = conf->output.separateScale ? conf->output.scaleY : conf->output.scale;
        int outW = static_cast<int>(inst.fbo_w * scaleX);
        int outH = static_cast<int>(inst.fbo_h * scaleY);

        int padding = (inst.fbo_w - conf->captureWidth) / 2;
        int paddingScaledX = static_cast<int>(padding * scaleX);
        int paddingScaledY = static_cast<int>(padding * scaleY);

        int finalScreenY_gl = fullH - finalScreenY_win - outH;

        glUniform4f(g_solidColorShaderLocs.color, outputColor.r, outputColor.g, outputColor.b, 1.0f);
        float x1 = (static_cast<float>(finalScreenX + paddingScaledX) / fullW) * 2.0f - 1.0f;
        float y1 = (static_cast<float>(finalScreenY_gl + paddingScaledY) / fullH) * 2.0f - 1.0f;
        float x2 = (static_cast<float>(finalScreenX + outW - paddingScaledX) / fullW) * 2.0f - 1.0f;
        float y2 = (static_cast<float>(finalScreenY_gl + outH - paddingScaledY) / fullH) * 2.0f - 1.0f;
        float v[] = { x1, y1, x2, y1, x2, y2, x1, y2 };
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(v), v);
        glDrawArrays(GL_LINE_LOOP, 0, 4);
    }

    glBindVertexArray(originalVAO);
}

void InitializeOverlayTextFont(const std::string& fontPath, float baseFontSize, float scaleFactor) {
    if (!ImGui::GetCurrentContext()) return;

    ImGuiIO& io = ImGui::GetIO();
    const float sizePixels = baseFontSize * 1.5f * scaleFactor;

    std::string usePath = fontPath.empty() ? ConfigDefaults::CONFIG_FONT_PATH : fontPath;

    auto isStable = [](const std::string& p, float sz) -> bool {
        if (p.empty()) return false;
        ImFontAtlas testAtlas;
        ImFont* f = testAtlas.AddFontFromFileTTF(p.c_str(), sz);
        if (!f) return false;
        return testAtlas.Build();
    };

    if (!isStable(usePath, sizePixels)) { usePath = ConfigDefaults::CONFIG_FONT_PATH; }

    g_overlayTextFont = io.Fonts->AddFontFromFileTTF(usePath.c_str(), sizePixels);
    if (!g_overlayTextFont && usePath != ConfigDefaults::CONFIG_FONT_PATH) {
        g_overlayTextFont = io.Fonts->AddFontFromFileTTF(ConfigDefaults::CONFIG_FONT_PATH.c_str(), sizePixels);
    }
    if (!g_overlayTextFont) {
        g_overlayTextFont = io.Fonts->AddFontDefault();
    }

    (void)io.Fonts->Build();
}

void SetOverlayTextFontSize(int sizePixels) {
    if (sizePixels < 1) sizePixels = 1;
    if (sizePixels > 512) sizePixels = 512;
    g_overlayTextFontSize = static_cast<float>(sizePixels);
}


void RenderTextureGridOverlay(bool showTextureGrid, int modeWidth, int modeHeight) {

    static bool loggedOnce = false;
    if (!loggedOnce) {
        Log("RenderTextureGridOverlay called - g_glInitialized: " +
            std::to_string(g_glInitialized.load(std::memory_order_acquire) ? 1 : 0) +
            ", g_solidColorProgram: " + std::to_string(g_solidColorProgram));
        loggedOnce = true;
    }

    if (!g_glInitialized.load(std::memory_order_acquire) || g_solidColorProgram == 0) { return; }

    const int MAX_TEXTURE_ID = 100;
    const int TILE_SIZE = 48;
    const int PADDING = 80;
    const int MARGIN = 80;

    int screenW = GetCachedWindowWidth();
    int screenH = GetCachedWindowHeight();

    struct TexInfo { GLuint id; GLint width; GLint height; GLint internalFormat; };
    std::vector<TexInfo> validTextures;
    for (GLuint id = 0; id <= MAX_TEXTURE_ID; id++) {
        if (glIsTexture(id)) {
            glBindTexture(GL_TEXTURE_2D, id);
            GLint texWidth = 0, texHeight = 0, internalFormat = 0;
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &texWidth);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &texHeight);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &internalFormat);

            if (modeWidth > 0 && modeHeight > 0) {
                if (texWidth == modeWidth && texHeight == modeHeight && internalFormat == GL_RGBA8) {
                    validTextures.push_back({ id, texWidth, texHeight, internalFormat });
                }
            } else {
                validTextures.push_back({ id, texWidth, texHeight, internalFormat });
            }
        }
    }

    if (validTextures.empty()) { return; }

    {
        std::lock_guard<std::mutex> lock(s_textureGridMutex);
        s_textureGridLabels.clear();
    }

    int tilesPerRow = (screenW - 2 * MARGIN) / (TILE_SIZE + PADDING);
    if (tilesPerRow < 1) tilesPerRow = 1;

    GLint lastProgram, lastTexture, lastVAO, lastArrayBuffer, lastActiveTexture;
    GLint lastBlendSrc, lastBlendDst;
    GLint lastMinFilter, lastMagFilter;
    GLboolean blendEnabled, depthEnabled;
    glGetIntegerv(GL_CURRENT_PROGRAM, &lastProgram);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &lastTexture);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &lastVAO);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &lastArrayBuffer);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &lastActiveTexture);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &lastBlendSrc);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &lastBlendDst);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &lastMinFilter);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &lastMagFilter);
    blendEnabled = glIsEnabled(GL_BLEND);
    depthEnabled = glIsEnabled(GL_DEPTH_TEST);

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(g_imageRenderProgram);
    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glActiveTexture(GL_TEXTURE0);

    glUniform1i(g_imageRenderShaderLocs.imageTexture, 0);
    glUniform1i(g_imageRenderShaderLocs.enableColorKey, 0);
    glUniform1f(g_imageRenderShaderLocs.opacity, 1.0f);

    std::unordered_map<GLuint, std::pair<GLint, GLint>> texFilterStates;

    int col = 0;
    int row = 0;
    for (const auto& tex : validTextures) {
        int x = MARGIN + col * (TILE_SIZE + PADDING);
        int y = MARGIN + row * (TILE_SIZE + PADDING);

        glBindTexture(GL_TEXTURE_2D, tex.id);

        GLint texWidth = tex.width;
        GLint texHeight = tex.height;
        GLint internalFormat = tex.internalFormat;

        GLint minFilter = 0, magFilter = 0, wrapS = 0, wrapT = 0;
        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &minFilter);
        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &magFilter);
        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, &wrapS);
        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, &wrapT);

        float sizeMB = (texWidth * texHeight * 4) / (1024.0f * 1024.0f);

        {
            std::lock_guard<std::mutex> lock(s_textureGridMutex);
            s_textureGridLabels.push_back({ tex.id, (float)x, (float)y, TILE_SIZE, texWidth, texHeight, sizeMB, (GLenum)internalFormat,
                                            minFilter, magFilter, wrapS, wrapT });
        }

        texFilterStates[tex.id] = { minFilter, magFilter };

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        float x1_ndc = (x / (float)screenW) * 2.0f - 1.0f;
        float x2_ndc = ((x + TILE_SIZE) / (float)screenW) * 2.0f - 1.0f;

        int y_gl = screenH - y - TILE_SIZE;
        float y1_ndc = (y_gl / (float)screenH) * 2.0f - 1.0f;
        float y2_ndc = ((y_gl + TILE_SIZE) / (float)screenH) * 2.0f - 1.0f;

        float verts[] = {
            x1_ndc, y1_ndc, 0.0f, 0.0f,
            x2_ndc, y1_ndc, 1.0f, 0.0f,
            x2_ndc, y2_ndc, 1.0f, 1.0f,
            x1_ndc, y1_ndc, 0.0f, 0.0f,
            x2_ndc, y2_ndc, 1.0f, 1.0f,
            x1_ndc, y2_ndc, 0.0f, 1.0f
        };

        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        col++;
        if (col >= tilesPerRow) {
            col = 0;
            row++;
        }
    }

    for (const auto& pair : texFilterStates) {
        glBindTexture(GL_TEXTURE_2D, pair.first);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, pair.second.first);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, pair.second.second);
    }

    glActiveTexture(lastActiveTexture);
    glBindTexture(GL_TEXTURE_2D, lastTexture);
    glBindVertexArray(lastVAO);
    glBindBuffer(GL_ARRAY_BUFFER, lastArrayBuffer);
    glUseProgram(lastProgram);

    if (depthEnabled)
        glEnable(GL_DEPTH_TEST);
    else
        glDisable(GL_DEPTH_TEST);

    if (blendEnabled) {
        glEnable(GL_BLEND);
        glBlendFunc(lastBlendSrc, lastBlendDst);
    } else {
        glDisable(GL_BLEND);
    }
}

void RenderCachedTextureGridLabels() {
    std::lock_guard<std::mutex> lock(s_textureGridMutex);

    if (!ImGui::GetCurrentContext() || s_textureGridLabels.empty()) { return; }

    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    float fontSize = ImGui::GetFontSize();

    for (const auto& label : s_textureGridLabels) {
        char idText[32];
        sprintf_s(idText, "ID: %u", label.textureId);

        char resText[64];
        sprintf_s(resText, "%dx%d", label.width, label.height);

        char sizeText[32];
        sprintf_s(sizeText, "%.2f MB", label.sizeMB);

        const char* formatStr = "UNK";
        if (label.internalFormat == GL_RGBA8)
            formatStr = "RGBA8";
        else if (label.internalFormat == GL_RGB8)
            formatStr = "RGB8";
        else if (label.internalFormat == GL_RGBA)
            formatStr = "RGBA";
        else if (label.internalFormat == GL_RGB)
            formatStr = "RGB";

        char formatText[32];
        sprintf_s(formatText, "Fmt: %s", formatStr);

        const char* minFilterStr = "?";
        if (label.minFilter == GL_NEAREST)
            minFilterStr = "N";
        else if (label.minFilter == GL_LINEAR)
            minFilterStr = "L";
        else if (label.minFilter == GL_NEAREST_MIPMAP_NEAREST)
            minFilterStr = "NMN";
        else if (label.minFilter == GL_LINEAR_MIPMAP_NEAREST)
            minFilterStr = "LMN";
        else if (label.minFilter == GL_NEAREST_MIPMAP_LINEAR)
            minFilterStr = "NML";
        else if (label.minFilter == GL_LINEAR_MIPMAP_LINEAR)
            minFilterStr = "LML";

        const char* magFilterStr = "?";
        if (label.magFilter == GL_NEAREST)
            magFilterStr = "N";
        else if (label.magFilter == GL_LINEAR)
            magFilterStr = "L";

        char filterText[32];
        sprintf_s(filterText, "F:%s/%s", minFilterStr, magFilterStr);

        const char* wrapSStr = "?";
        if (label.wrapS == GL_REPEAT)
            wrapSStr = "R";
        else if (label.wrapS == GL_CLAMP_TO_EDGE)
            wrapSStr = "C";
        else if (label.wrapS == GL_MIRRORED_REPEAT)
            wrapSStr = "M";
        else if (label.wrapS == GL_CLAMP_TO_BORDER)
            wrapSStr = "B";

        const char* wrapTStr = "?";
        if (label.wrapT == GL_REPEAT)
            wrapTStr = "R";
        else if (label.wrapT == GL_CLAMP_TO_EDGE)
            wrapTStr = "C";
        else if (label.wrapT == GL_MIRRORED_REPEAT)
            wrapTStr = "M";
        else if (label.wrapT == GL_CLAMP_TO_BORDER)
            wrapTStr = "B";

        char wrapText[32];
        sprintf_s(wrapText, "W:%s/%s", wrapSStr, wrapTStr);

        const char* lines[] = { idText, resText, sizeText, formatText, filterText, wrapText };
        const int lineCount = 6;
        float lineSpacing = 2.0f;

        float currentY = label.y + 2.0f;
        for (int i = 0; i < lineCount; i++) {
            ImVec2 textSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, lines[i]);
            ImVec2 textPos(label.x + (label.tileSize - textSize.x) / 2.0f, currentY);

            ImVec2 bgMin(textPos.x - 2, textPos.y - 1);
            ImVec2 bgMax(textPos.x + textSize.x + 2, textPos.y + textSize.y + 1);
            drawList->AddRectFilled(bgMin, bgMax, IM_COL32(0, 0, 0, 180));

            drawList->AddText(textPos, IM_COL32(255, 255, 255, 255), lines[i]);

            currentY += textSize.y + lineSpacing;
        }
    }
}

static float EaseOutPower(float t, float power) {
    float t1 = t - 1.0f;
    float sign = (t1 < 0) ? -1.0f : 1.0f;
    return sign * std::pow(std::abs(t1), power) + 1.0f;
}

static float EaseInPower(float t, float power) { return std::pow(t, power); }

static float ApplyDualEasing(float t, float easeInPower, float easeOutPower) {
    easeInPower = std::clamp(easeInPower, 1.0f, 10.0f);
    easeOutPower = std::clamp(easeOutPower, 1.0f, 10.0f);

    if (easeInPower <= 1.0f && easeOutPower <= 1.0f) { return t; }

    if (t < 0.5f) {
        float halfT = t * 2.0f;
        float easedHalfT = EaseInPower(halfT, easeInPower);
        return easedHalfT * 0.5f;
    } else {
        float halfT = (t - 0.5f) * 2.0f;
        float easedHalfT = EaseOutPower(halfT, easeOutPower);
        return 0.5f + easedHalfT * 0.5f;
    }
}

static float CalculateBounceOffset(float bounceProgress, int bounceIndex, int totalBounces, float intensity) {
    if (totalBounces <= 0 || bounceIndex >= totalBounces) return 0.0f;

    float decayFactor = 1.0f - (static_cast<float>(bounceIndex) / static_cast<float>(totalBounces));
    decayFactor = decayFactor * decayFactor;

    float angle = bounceProgress * 3.14159265f;
    float bounce = std::sin(angle) * intensity * decayFactor;

    return bounce;
}

void StartModeTransition(const std::string& fromModeId, const std::string& toModeId, int fromWidth, int fromHeight, int fromX, int fromY,
                         int toWidth, int toHeight, int toX, int toY, const ModeConfig& toMode) {
    LogCategory("animation", "[ANIMATION] StartModeTransition entry - acquiring g_modeTransitionMutex...");
    std::lock_guard<std::mutex> lock(g_modeTransitionMutex);
    LogCategory("animation", "[ANIMATION] g_modeTransitionMutex acquired");

    bool transitioningToFullscreen = EqualsIgnoreCase(toModeId, "Fullscreen");
    bool transitioningFromFullscreen = EqualsIgnoreCase(fromModeId, "Fullscreen");

    bool isAllCutTransition = toMode.gameTransition == GameTransitionType::Cut && toMode.overlayTransition == OverlayTransitionType::Cut &&
                              toMode.backgroundTransition == BackgroundTransitionType::Cut;

    if (isAllCutTransition && !transitioningToFullscreen) {
        LogCategory("animation", "[ANIMATION] Cut/Cut/Cut transition - using 1-frame protection to prevent black flash");
    }

    g_modeTransition.active = true;
    g_modeTransition.startTime = std::chrono::steady_clock::now();

    bool allCutToFullscreen = transitioningToFullscreen && toMode.gameTransition == GameTransitionType::Cut;
    bool allCutWithFirstFrameProtection = isAllCutTransition && !transitioningToFullscreen;
    if (allCutToFullscreen || allCutWithFirstFrameProtection) {
        g_modeTransition.duration = 0.001f;
    } else {
        g_modeTransition.duration = toMode.transitionDurationMs / 1000.0f;
    }

    g_modeTransition.gameTransition = toMode.gameTransition;
    g_modeTransition.overlayTransition = OverlayTransitionType::Cut;
    g_modeTransition.backgroundTransition = BackgroundTransitionType::Cut;

    g_modeTransition.easeInPower = toMode.easeInPower;
    g_modeTransition.easeOutPower = toMode.easeOutPower;
    g_modeTransition.bounceCount = toMode.bounceCount;
    g_modeTransition.bounceIntensity = toMode.bounceIntensity;
    g_modeTransition.bounceDurationMs = toMode.bounceDurationMs;

    bool transitioningToEyeZoom = EqualsIgnoreCase(toModeId, "EyeZoom");
    bool transitioningFromEyeZoom = EqualsIgnoreCase(fromModeId, "EyeZoom");

    if (transitioningFromEyeZoom && !transitioningToEyeZoom) {
        // Transitioning FROM EyeZoom - look up EyeZoom's skip settings (use snapshot for thread safety)
        auto transSnap = GetConfigSnapshot();
        const ModeConfig* eyeZoomMode = transSnap ? GetModeFromSnapshot(*transSnap, "EyeZoom") : nullptr;
        if (eyeZoomMode) {
            g_modeTransition.skipAnimateX = eyeZoomMode->skipAnimateX;
            g_modeTransition.skipAnimateY = eyeZoomMode->skipAnimateY;
        }
    } else {
        g_modeTransition.skipAnimateX = toMode.skipAnimateX;
        g_modeTransition.skipAnimateY = toMode.skipAnimateY;
    }

    g_modeTransition.fromModeId = fromModeId;
    g_modeTransition.fromWidth = fromWidth;
    g_modeTransition.fromHeight = fromHeight;
    g_modeTransition.fromX = fromX;
    g_modeTransition.fromY = fromY;

    g_modeTransition.toModeId = toModeId;
    g_modeTransition.toWidth = toWidth;
    g_modeTransition.toHeight = toHeight;
    g_modeTransition.toX = toX;
    g_modeTransition.toY = toY;

    // Use snapshot for thread-safe lookup of fromMode (called from multiple threads)
    auto nativeSnap = GetConfigSnapshot();
    const ModeConfig* fromModePtr = nativeSnap ? GetModeFromSnapshot(*nativeSnap, fromModeId) : nullptr;
    if (fromModePtr) {
        g_modeTransition.fromNativeWidth = fromModePtr->width;
        g_modeTransition.fromNativeHeight = fromModePtr->height;
    } else {
        g_modeTransition.fromNativeWidth = fromWidth;
        g_modeTransition.fromNativeHeight = fromHeight;
    }
    g_modeTransition.toNativeWidth = toMode.width > 0 ? toMode.width : toWidth;
    g_modeTransition.toNativeHeight = toMode.height > 0 ? toMode.height : toHeight;

    if (toMode.gameTransition == GameTransitionType::Bounce) {
        g_modeTransition.currentWidth = fromWidth;
        g_modeTransition.currentHeight = fromHeight;
        g_modeTransition.currentX = fromX;
        g_modeTransition.currentY = fromY;
    } else {
        g_modeTransition.currentWidth = toWidth;
        g_modeTransition.currentHeight = toHeight;
        g_modeTransition.currentX = toX;
        g_modeTransition.currentY = toY;
    }
    g_modeTransition.progress = 0.0f;
    g_modeTransition.moveProgress = 0.0f; // Must initialize to 0 for first frame overlay positioning
    g_modeTransition.wmSizeSent = false;

    g_modeTransition.lastSentWidth = 0;
    g_modeTransition.lastSentHeight = 0;

    if (transitioningFromEyeZoom && !transitioningToEyeZoom) {
        g_isTransitioningFromEyeZoom.store(true, std::memory_order_release);
        LogCategory("animation", "[ANIMATION] Set g_isTransitioningFromEyeZoom=true BEFORE WM_SIZE to freeze snapshot");
    } else {
        g_isTransitioningFromEyeZoom.store(false, std::memory_order_release);
    }

    int wmWidth = toMode.width > 0 ? toMode.width : toWidth;
    int wmHeight = toMode.height > 0 ? toMode.height : toHeight;

    HWND hwnd = g_minecraftHwnd.load();
    if (hwnd && wmWidth > 0 && wmHeight > 0) {
        PostMessage(hwnd, WM_SIZE, SIZE_RESTORED, MAKELPARAM(wmWidth, wmHeight));
        g_modeTransition.wmSizeSent = true;
        g_modeTransition.lastSentWidth = wmWidth;
        g_modeTransition.lastSentHeight = wmHeight;
        LogCategory("animation", "[ANIMATION] WM_SIZE sent immediately: " + std::to_string(wmWidth) + "x" + std::to_string(wmHeight));
    }

    LogCategory("animation", "[ANIMATION] Starting mode transition (Game:" + GameTransitionTypeToString(toMode.gameTransition) +
                                 ", Overlay:" + OverlayTransitionTypeToString(toMode.overlayTransition) +
                                 ", Bg:" + BackgroundTransitionTypeToString(toMode.backgroundTransition) + ", " +
                                 std::to_string(toMode.transitionDurationMs) + "ms): " + fromModeId + " (" + std::to_string(fromWidth) +
                                 "x" + std::to_string(fromHeight) + " at " + std::to_string(fromX) + "," + std::to_string(fromY) + ")" +
                                 " -> " + toModeId + " (" + std::to_string(toWidth) + "x" + std::to_string(toHeight) + " at " +
                                 std::to_string(toX) + "," + std::to_string(toY) + ")");

    // Update lock-free snapshot for viewport hook and GetModeTransitionState (done inside the lock)
    int nextSnapshotIndex = 1 - g_viewportTransitionSnapshotIndex.load(std::memory_order_relaxed);
    ViewportTransitionSnapshot& snapshot = g_viewportTransitionSnapshots[nextSnapshotIndex];
    snapshot.active = g_modeTransition.active;
    snapshot.isBounceTransition = (g_modeTransition.gameTransition == GameTransitionType::Bounce);
    snapshot.fromModeId = g_modeTransition.fromModeId;
    snapshot.toModeId = g_modeTransition.toModeId;
    snapshot.fromWidth = g_modeTransition.fromWidth;
    snapshot.fromHeight = g_modeTransition.fromHeight;
    snapshot.fromX = g_modeTransition.fromX;
    snapshot.fromY = g_modeTransition.fromY;
    snapshot.currentX = g_modeTransition.currentX;
    snapshot.currentY = g_modeTransition.currentY;
    snapshot.currentWidth = g_modeTransition.currentWidth;
    snapshot.currentHeight = g_modeTransition.currentHeight;
    snapshot.toX = g_modeTransition.toX;
    snapshot.toY = g_modeTransition.toY;
    snapshot.toWidth = g_modeTransition.toWidth;
    snapshot.toHeight = g_modeTransition.toHeight;
    snapshot.fromNativeWidth = g_modeTransition.fromNativeWidth;
    snapshot.fromNativeHeight = g_modeTransition.fromNativeHeight;
    snapshot.toNativeWidth = g_modeTransition.toNativeWidth;
    snapshot.toNativeHeight = g_modeTransition.toNativeHeight;
    snapshot.gameTransition = g_modeTransition.gameTransition;
    snapshot.overlayTransition = g_modeTransition.overlayTransition;
    snapshot.backgroundTransition = g_modeTransition.backgroundTransition;
    snapshot.progress = g_modeTransition.progress;
    snapshot.moveProgress = g_modeTransition.moveProgress;
    snapshot.startTime = g_modeTransition.startTime;
    g_viewportTransitionSnapshotIndex.store(nextSnapshotIndex, std::memory_order_release);

    LogCategory("animation", "[ANIMATION] StartModeTransition complete - releasing g_modeTransitionMutex");
}

void UpdateModeTransition() {
    std::lock_guard<std::mutex> lock(g_modeTransitionMutex);

    if (!g_modeTransition.active) return;

    auto now = std::chrono::steady_clock::now();
    float elapsed = std::chrono::duration<float>(now - g_modeTransition.startTime).count();

    float baseDuration = g_modeTransition.duration;
    float totalBounceDuration =
        (g_modeTransition.bounceCount > 0) ? (g_modeTransition.bounceCount * g_modeTransition.bounceDurationMs / 1000.0f) : 0.0f;
    float totalDuration = baseDuration + totalBounceDuration;

    float progress = elapsed / totalDuration;
    g_modeTransition.progress = (progress < 1.0f) ? progress : 1.0f;

    if (g_modeTransition.gameTransition == GameTransitionType::Bounce) {
        float baseRatio = baseDuration / totalDuration;

        float moveProgress = 0.0f;
        float bounceOffset = 0.0f;

        if (g_modeTransition.progress < baseRatio) {
            float phaseProgress = g_modeTransition.progress / baseRatio;
            moveProgress = std::clamp(phaseProgress, 0.0f, 1.0f);
            moveProgress = ApplyDualEasing(moveProgress, g_modeTransition.easeInPower, g_modeTransition.easeOutPower);
        } else {
            moveProgress = 1.0f;

            if (g_modeTransition.bounceCount > 0 && totalBounceDuration > 0) {
                float bounceElapsed = (g_modeTransition.progress - baseRatio) * totalDuration;
                float singleBounceDuration = g_modeTransition.bounceDurationMs / 1000.0f;

                int currentBounce = static_cast<int>(bounceElapsed / singleBounceDuration);
                if (currentBounce < g_modeTransition.bounceCount) {
                    float bouncePhaseProgress = std::fmod(bounceElapsed, singleBounceDuration) / singleBounceDuration;
                    bounceOffset = CalculateBounceOffset(bouncePhaseProgress, currentBounce, g_modeTransition.bounceCount,
                                                         g_modeTransition.bounceIntensity);
                }
            }
        }

        int baseWidth, baseHeight, baseX, baseY;

        if (g_modeTransition.fromWidth == g_modeTransition.toWidth) {
            baseWidth = g_modeTransition.toWidth;
        } else {
            baseWidth =
                static_cast<int>(g_modeTransition.fromWidth + (g_modeTransition.toWidth - g_modeTransition.fromWidth) * moveProgress);
        }

        if (g_modeTransition.fromHeight == g_modeTransition.toHeight) {
            baseHeight = g_modeTransition.toHeight;
        } else {
            baseHeight =
                static_cast<int>(g_modeTransition.fromHeight + (g_modeTransition.toHeight - g_modeTransition.fromHeight) * moveProgress);
        }

        if (g_modeTransition.fromX == g_modeTransition.toX) {
            baseX = g_modeTransition.toX;
        } else {
            baseX = static_cast<int>(g_modeTransition.fromX + (g_modeTransition.toX - g_modeTransition.fromX) * moveProgress);
        }

        if (g_modeTransition.fromY == g_modeTransition.toY) {
            baseY = g_modeTransition.toY;
        } else {
            baseY = static_cast<int>(g_modeTransition.fromY + (g_modeTransition.toY - g_modeTransition.fromY) * moveProgress);
        }

        if (g_modeTransition.skipAnimateX) {
            baseWidth = g_modeTransition.toWidth;
            baseX = g_modeTransition.toX;
        }
        if (g_modeTransition.skipAnimateY) {
            baseHeight = g_modeTransition.toHeight;
            baseY = g_modeTransition.toY;
        }

        if (bounceOffset != 0.0f) {
            int deltaW = g_modeTransition.toWidth - g_modeTransition.fromWidth;
            int deltaH = g_modeTransition.toHeight - g_modeTransition.fromHeight;
            bool skipBounceX = g_modeTransition.skipAnimateX ||
                               (g_modeTransition.fromWidth == g_modeTransition.toWidth && g_modeTransition.fromX == g_modeTransition.toX);
            if (skipBounceX) {
                g_modeTransition.currentWidth = g_modeTransition.toWidth;
                g_modeTransition.currentX = g_modeTransition.toX;
            } else {
                g_modeTransition.currentWidth = g_modeTransition.toWidth - static_cast<int>(deltaW * bounceOffset);
                int deltaX = g_modeTransition.toX - g_modeTransition.fromX;
                g_modeTransition.currentX = g_modeTransition.toX - static_cast<int>(deltaX * bounceOffset);
            }
            bool skipBounceY = g_modeTransition.skipAnimateY ||
                               (g_modeTransition.fromHeight == g_modeTransition.toHeight && g_modeTransition.fromY == g_modeTransition.toY);
            if (skipBounceY) {
                g_modeTransition.currentHeight = g_modeTransition.toHeight;
                g_modeTransition.currentY = g_modeTransition.toY;
            } else {
                g_modeTransition.currentHeight = g_modeTransition.toHeight - static_cast<int>(deltaH * bounceOffset);
                int deltaY = g_modeTransition.toY - g_modeTransition.fromY;
                g_modeTransition.currentY = g_modeTransition.toY - static_cast<int>(deltaY * bounceOffset);
            }
        } else {
            g_modeTransition.currentWidth = baseWidth;
            g_modeTransition.currentHeight = baseHeight;
            g_modeTransition.currentX = baseX;
            g_modeTransition.currentY = baseY;
        }

        g_modeTransition.moveProgress = moveProgress;
    } else {
        g_modeTransition.moveProgress = g_modeTransition.progress;
    }


    bool allComplete = (elapsed >= totalDuration);

    if (allComplete) {
        LogCategory("animation", "[ANIMATION] Mode transition complete: " + g_modeTransition.toModeId + " (final stretch: " +
                                     std::to_string(g_modeTransition.toWidth) + "x" + std::to_string(g_modeTransition.toHeight) + " at " +
                                     std::to_string(g_modeTransition.toX) + "," + std::to_string(g_modeTransition.toY) + ")");

        g_modeTransition.currentWidth = g_modeTransition.toWidth;
        g_modeTransition.currentHeight = g_modeTransition.toHeight;
        g_modeTransition.currentX = g_modeTransition.toX;
        g_modeTransition.currentY = g_modeTransition.toY;

        g_modeTransition.active = false;
    }

    // Update lock-free snapshot for viewport hook and GetModeTransitionState (done inside the lock)
    int nextSnapshotIndex = 1 - g_viewportTransitionSnapshotIndex.load(std::memory_order_relaxed);
    ViewportTransitionSnapshot& snapshot = g_viewportTransitionSnapshots[nextSnapshotIndex];
    snapshot.active = g_modeTransition.active;
    snapshot.isBounceTransition = (g_modeTransition.gameTransition == GameTransitionType::Bounce);
    snapshot.fromModeId = g_modeTransition.fromModeId;
    snapshot.toModeId = g_modeTransition.toModeId;
    snapshot.fromWidth = g_modeTransition.fromWidth;
    snapshot.fromHeight = g_modeTransition.fromHeight;
    snapshot.fromX = g_modeTransition.fromX;
    snapshot.fromY = g_modeTransition.fromY;
    snapshot.currentX = g_modeTransition.currentX;
    snapshot.currentY = g_modeTransition.currentY;
    snapshot.currentWidth = g_modeTransition.currentWidth;
    snapshot.currentHeight = g_modeTransition.currentHeight;
    snapshot.toX = g_modeTransition.toX;
    snapshot.toY = g_modeTransition.toY;
    snapshot.toWidth = g_modeTransition.toWidth;
    snapshot.toHeight = g_modeTransition.toHeight;
    snapshot.fromNativeWidth = g_modeTransition.fromNativeWidth;
    snapshot.fromNativeHeight = g_modeTransition.fromNativeHeight;
    snapshot.toNativeWidth = g_modeTransition.toNativeWidth;
    snapshot.toNativeHeight = g_modeTransition.toNativeHeight;
    snapshot.gameTransition = g_modeTransition.gameTransition;
    snapshot.overlayTransition = g_modeTransition.overlayTransition;
    snapshot.backgroundTransition = g_modeTransition.backgroundTransition;
    snapshot.progress = g_modeTransition.progress;
    snapshot.moveProgress = g_modeTransition.moveProgress;
    snapshot.startTime = g_modeTransition.startTime;
    g_viewportTransitionSnapshotIndex.store(nextSnapshotIndex, std::memory_order_release);
}

bool IsModeTransitionActive() {
    std::lock_guard<std::mutex> lock(g_modeTransitionMutex);
    return g_modeTransition.active;
}

GameTransitionType GetGameTransitionType() {
    std::lock_guard<std::mutex> lock(g_modeTransitionMutex);
    return g_modeTransition.active ? g_modeTransition.gameTransition : GameTransitionType::Cut;
}

OverlayTransitionType GetOverlayTransitionType() {
    std::lock_guard<std::mutex> lock(g_modeTransitionMutex);
    return g_modeTransition.active ? g_modeTransition.overlayTransition : OverlayTransitionType::Cut;
}

BackgroundTransitionType GetBackgroundTransitionType() {
    std::lock_guard<std::mutex> lock(g_modeTransitionMutex);
    return g_modeTransition.active ? g_modeTransition.backgroundTransition : BackgroundTransitionType::Cut;
}

std::string GetModeTransitionFromModeId() {
    std::lock_guard<std::mutex> lock(g_modeTransitionMutex);
    return g_modeTransition.active ? g_modeTransition.fromModeId : "";
}

void GetAnimatedModeViewport(int& outWidth, int& outHeight) {
    std::lock_guard<std::mutex> lock(g_modeTransitionMutex);
    if (g_modeTransition.active) {
        outWidth = g_modeTransition.currentWidth;
        outHeight = g_modeTransition.currentHeight;
    } else {
        ModeViewportInfo viewport = GetCurrentModeViewport();
        if (viewport.valid) {
            outWidth = viewport.stretchEnabled ? viewport.stretchWidth : viewport.width;
            outHeight = viewport.stretchEnabled ? viewport.stretchHeight : viewport.height;
        } else {
            outWidth = GetCachedWindowWidth();
            outHeight = GetCachedWindowHeight();
        }
    }
}

void GetAnimatedModePosition(int& outX, int& outY) {
    std::lock_guard<std::mutex> lock(g_modeTransitionMutex);
    if (g_modeTransition.active) {
        outX = g_modeTransition.currentX;
        outY = g_modeTransition.currentY;
    } else {
        ModeViewportInfo viewport = GetCurrentModeViewport();
        if (viewport.valid) {
            outX = viewport.stretchX;
            outY = viewport.stretchY;
        } else {
            int screenW = GetCachedWindowWidth();
            int screenH = GetCachedWindowHeight();
            outX = screenW / 2;
            outY = screenH / 2;
        }
    }
}

bool WaitForOverlayBlitFence() {
    GLsync fence = g_overlayBlitFence.exchange(nullptr, std::memory_order_acq_rel);
    if (fence) {
        glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
        glDeleteSync(fence);
        return true;
    }
    return false;
}

ModeTransitionState GetModeTransitionState() {
    // Lock-free read from double-buffered snapshot
    const ViewportTransitionSnapshot& snapshot =
        g_viewportTransitionSnapshots[g_viewportTransitionSnapshotIndex.load(std::memory_order_acquire)];

    ModeTransitionState state;
    state.active = snapshot.active;
    if (state.active) {
        state.width = snapshot.currentWidth;
        state.height = snapshot.currentHeight;
        state.x = snapshot.currentX;
        state.y = snapshot.currentY;
        state.gameTransition = snapshot.gameTransition;
        state.overlayTransition = snapshot.overlayTransition;
        state.backgroundTransition = snapshot.backgroundTransition;
        state.progress = snapshot.progress;
        state.moveProgress = snapshot.moveProgress;

        state.targetWidth = snapshot.toWidth;
        state.targetHeight = snapshot.toHeight;
        state.targetX = snapshot.toX;
        state.targetY = snapshot.toY;
        state.fromWidth = snapshot.fromWidth;
        state.fromHeight = snapshot.fromHeight;
        state.fromX = snapshot.fromX;
        state.fromY = snapshot.fromY;
        state.fromModeId = snapshot.fromModeId;
    } else {
        state.width = 0;
        state.height = 0;
        state.x = 0;
        state.y = 0;
        state.gameTransition = GameTransitionType::Cut;
        state.overlayTransition = OverlayTransitionType::Cut;
        state.backgroundTransition = BackgroundTransitionType::Cut;
        state.progress = 1.0f;
        state.moveProgress = 1.0f;
        state.targetWidth = 0;
        state.targetHeight = 0;
        state.targetX = 0;
        state.targetY = 0;
        state.fromWidth = 0;
        state.fromHeight = 0;
        state.fromX = 0;
        state.fromY = 0;
    }
    return state;
}


