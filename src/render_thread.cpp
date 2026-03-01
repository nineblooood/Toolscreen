#include "render_thread.h"
#include "fake_cursor.h"
#include "gui.h"
#include "imgui_input_queue.h"
#include "mirror_thread.h"
#include "obs_thread.h"
#include "profiler.h"
#include "render.h"
#include "shared_contexts.h"
#include "stb_image.h"
#include "utils.h"
#include "virtual_camera.h"
#include "window_overlay.h"
#include <unordered_map>
#include <set>
#include <thread>
#include <fstream>

// ImGui includes for render thread
#include "include/imgui/backends/imgui_impl_opengl3.h"
#include "include/imgui/backends/imgui_impl_win32.h"
#include "include/imgui/imgui.h"

#include "logic_thread.h"

static std::thread g_renderThread;
std::atomic<bool> g_renderThreadRunning{ false };
static std::atomic<bool> g_renderThreadShouldStop{ false };
std::atomic<uint64_t> g_renderFrameNumber{ 0 };

// OpenGL context for render thread
static HGLRC g_renderThreadContext = NULL;
static HDC g_renderThreadDC = NULL;
static bool g_renderContextIsShared = false;

// Fallback-mode DC ownership:
// Using the game's HDC on a different thread is undefined on some drivers and can cause
// intermittent SEH/AVs and black mirrors. Prefer a dedicated dummy window/DC.
static HWND g_renderFallbackDummyHwnd = NULL;
static HDC g_renderFallbackDummyDC = NULL;
static HWND g_renderOwnedDCHwnd = NULL; // Non-null when we called GetDC(hwnd) for g_renderThreadDC

static bool RT_CreateFallbackDummyWindowWithMatchingPixelFormat(HDC gameHdc, const wchar_t* windowNameTag, HWND& outHwnd, HDC& outDc) {
    if (outHwnd && outDc) { return true; }
    if (!gameHdc) { return false; }

    int gamePf = GetPixelFormat(gameHdc);
    if (gamePf == 0) { return false; }

    PIXELFORMATDESCRIPTOR gamePfd = {};
    gamePfd.nSize = sizeof(gamePfd);
    gamePfd.nVersion = 1;
    if (DescribePixelFormat(gameHdc, gamePf, sizeof(gamePfd), &gamePfd) == 0) { return false; }

    static ATOM s_atom = 0;
    if (!s_atom) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_OWNDC;
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.lpszClassName = L"ToolscreenRenderThreadDummy";
        s_atom = RegisterClassExW(&wc);
        if (!s_atom) {
            DWORD err = GetLastError();
            if (err != ERROR_CLASS_ALREADY_EXISTS) { return false; }
        }
    }

    std::wstring wndName = L"ToolscreenRenderThreadDummy_";
    wndName += (windowNameTag ? windowNameTag : L"render");

    outHwnd = CreateWindowExW(0, L"ToolscreenRenderThreadDummy", wndName.c_str(), WS_OVERLAPPED, 0, 0, 1, 1, NULL, NULL,
                              GetModuleHandleW(NULL), NULL);
    if (!outHwnd) { return false; }

    outDc = GetDC(outHwnd);
    if (!outDc) {
        DestroyWindow(outHwnd);
        outHwnd = NULL;
        return false;
    }

    if (!SetPixelFormat(outDc, gamePf, &gamePfd)) {
        ReleaseDC(outHwnd, outDc);
        DestroyWindow(outHwnd);
        outDc = NULL;
        outHwnd = NULL;
        return false;
    }
    return true;
}

struct RenderFBO {
    GLuint fbo = 0;
    GLuint texture = 0;
    GLuint stencilRbo = 0;
    int width = 0;
    int height = 0;
    std::atomic<bool> ready{ false };
    uint64_t frameNumber = 0;
    GLsync gpuFence = nullptr;        // Fence to ensure rendering is complete before reading
};

static RenderFBO g_renderFBOs[RENDER_THREAD_FBO_COUNT];
static std::atomic<int> g_writeFBOIndex{ 0 }; // FBO currently being written by render thread
static std::atomic<int> g_readFBOIndex{ -1 }; // FBO ready for reading by main thread (-1 = none ready)

// Consumer fences (main thread -> render thread):
// One fence per FBO index, created by the main thread after it finishes sampling that FBO's texture.
// The render thread waits on (and deletes) the fence before reusing the FBO as a render target.
static std::atomic<GLsync> g_renderFBOConsumerFences[RENDER_THREAD_FBO_COUNT];

static RenderFBO g_obsRenderFBOs[RENDER_THREAD_FBO_COUNT];
static std::atomic<int> g_obsWriteFBOIndex{ 0 };
static std::atomic<int> g_obsReadFBOIndex{ -1 };

static std::atomic<GLsync> g_obsFBOConsumerFences[RENDER_THREAD_FBO_COUNT];

static int FindFboIndexByTexture(const RenderFBO* fboArray, GLuint tex) {
    if (tex == 0) return -1;
    for (int i = 0; i < RENDER_THREAD_FBO_COUNT; ++i) {
        if (fboArray[i].texture == tex) return i;
    }
    return -1;
}

static void RT_WaitForConsumerFence(bool isObsRequest, int writeIdx) {
    if (writeIdx < 0 || writeIdx >= RENDER_THREAD_FBO_COUNT) return;

    std::atomic<GLsync>* fenceArray = isObsRequest ? g_obsFBOConsumerFences : g_renderFBOConsumerFences;
    GLsync consumer = fenceArray[writeIdx].exchange(nullptr, std::memory_order_acq_rel);
    if (consumer) {
        if (glIsSync(consumer)) { glWaitSync(consumer, 0, GL_TIMEOUT_IGNORED); }
        if (glIsSync(consumer)) { glDeleteSync(consumer); }
    }
}

// Last known good texture - updated only after GPU fence confirms rendering complete
static std::atomic<GLuint> g_lastGoodTexture{ 0 };
static std::atomic<GLuint> g_lastGoodObsTexture{ 0 };

// Fence for the last good texture - main thread can wait on this for synchronization
// This is more efficient than glFinish() as it only waits for the render thread's commands
static std::atomic<GLsync> g_lastGoodFence{ nullptr };
static std::atomic<GLsync> g_lastGoodObsFence{ nullptr };

// Ring buffer for deferred fence deletion - keeps fences alive for a while.
// This prevents a TOCTOU race where the main thread reads a fence pointer from
// GetCompletedRenderFence(), gets preempted, and then the render thread deletes
// that same fence after it has been rotated out.
static constexpr size_t FENCE_DELETION_DELAY = 64;
static GLsync g_pendingDeleteFences[FENCE_DELETION_DELAY] = { nullptr };
static GLsync g_pendingDeleteObsFences[FENCE_DELETION_DELAY] = { nullptr };
static size_t g_pendingDeleteIndex = 0;
static size_t g_pendingDeleteObsIndex = 0;

static GLuint g_virtualCamPBO = 0;
static int g_virtualCamPBOWidth = 0;
static int g_virtualCamPBOHeight = 0;
static bool g_virtualCamPBOPending = false;
static GLuint g_virtualCamCopyFBO = 0;

static GLuint g_vcComputeProgram = 0;
static GLuint g_vcScaleFBO = 0;
static GLuint g_vcScaleTexture = 0;
static int g_vcScaleWidth = 0;
static int g_vcScaleHeight = 0;
static bool g_vcUseCompute = false;

static GLuint g_vcYImage[2] = { 0, 0 };
static GLuint g_vcUVImage[2] = { 0, 0 };
static GLuint g_vcReadbackPBO[2] = { 0, 0 };
static GLuint g_vcReadbackFBO = 0;
static GLsync g_vcFence = nullptr;           // GPU fence after compute dispatch
static int g_vcWriteIdx = 0;
static int g_vcOutWidth = 0;
static int g_vcOutHeight = 0;
static bool g_vcComputePending = false;
static bool g_vcReadbackPending = false;

static GLuint g_vcCursorFBO = 0;
static GLuint g_vcCursorTexture = 0;
static int g_vcCursorWidth = 0;
static int g_vcCursorHeight = 0;

static GLint g_vcLocRgbaTexture = -1;
static GLint g_vcLocWidth = -1;
static GLint g_vcLocHeight = -1;

// Double-buffered request queue: main thread writes to one slot, render thread reads from other
// This allows lock-free submission - main thread never blocks waiting for render thread
static FrameRenderRequest g_requestSlots[2];
static std::atomic<int> g_requestWriteSlot{ 0 };    // Slot main thread writes to next
// the producer can lap and overwrite the slot the render thread is currently copying from
// (e.g. at very high FPS), which is a C++ data race and can manifest as a 1-frame "missing overlay".
static std::atomic<int> g_requestReadSlot{ -1 };    // Slot currently being copied by render thread (-1 = none)
// Producer stores with release AFTER fully writing the struct. Consumer exchanges with acq_rel
static std::atomic<int> g_requestReadySlot{ -1 };
static std::mutex g_requestSignalMutex;
static std::condition_variable g_requestCV;

static ObsFrameSubmission g_obsSubmissionSlots[2];
static std::atomic<int> g_obsWriteSlot{ 0 };
static std::atomic<int> g_obsReadSlot{ -1 }; // Slot currently being copied by render thread (-1 = none)
static std::atomic<int> g_obsReadySlot{ -1 };

static std::mutex g_completionMutex;
static std::condition_variable g_completionCV;
static std::atomic<bool> g_frameComplete{ false };

static std::mutex g_obsCompletionMutex;
static std::condition_variable g_obsCompletionCV;
static std::atomic<bool> g_obsFrameComplete{ false };

static GLuint rt_eyeZoomSnapshotTexture = 0;
static GLuint rt_eyeZoomSnapshotFBO = 0;
static int rt_eyeZoomSnapshotWidth = 0;
static int rt_eyeZoomSnapshotHeight = 0;
static bool rt_eyeZoomSnapshotValid = false;

static std::atomic<uint64_t> g_framesRendered{ 0 };
static std::atomic<uint64_t> g_framesDropped{ 0 };
static std::atomic<double> g_avgRenderTimeMs{ 0.0 };
static std::atomic<double> g_lastRenderTimeMs{ 0.0 };

extern std::atomic<HWND> g_minecraftHwnd;
extern std::atomic<bool> g_hwndChanged;

static ImGuiContext* g_renderThreadImGuiContext = nullptr;
static bool g_renderThreadImGuiInitialized = false;

std::atomic<bool> g_eyeZoomFontNeedsReload{ false };
static ImFont* g_eyeZoomTextFont = nullptr;
static std::string g_eyeZoomFontPathCached = "";
static float g_eyeZoomScaleFactor = 1.0f;
static bool g_fontsValid = false;

static bool RT_IsFontStable(const std::string& fontPath, float sizePixels) {
    (void)sizePixels;
    if (fontPath.empty()) return false;

    // can crash in release builds. Therefore, RT_IsFontStable must NOT call AddFontFromFileTTF.
    const DWORD attrs = GetFileAttributesA(fontPath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        return false;
    }

    std::ifstream f(fontPath, std::ios::binary);
    if (!f) return false;

    unsigned char sig[4] = { 0, 0, 0, 0 };
    f.read(reinterpret_cast<char*>(sig), sizeof(sig));
    if (!f) return false;

    const unsigned char ttfSig[4] = { 0x00, 0x01, 0x00, 0x00 };
    if (memcmp(sig, ttfSig, 4) == 0) return true;
    if (memcmp(sig, "OTTO", 4) == 0) return true;
    if (memcmp(sig, "ttcf", 4) == 0) return true;
    if (memcmp(sig, "true", 4) == 0) return true;
    if (memcmp(sig, "typ1", 4) == 0) return true;

    return false;
}

static ImFont* RT_SafeAddFontFromFileTTF(ImFontAtlas* atlas, const char* path, float sizePixels,
                                        const ImFontConfig* fontCfg = nullptr, const ImWchar* glyphRanges = nullptr) {
    if (!atlas || !path || !path[0]) return nullptr;
    ImFont* font = nullptr;
    __try {
        font = atlas->AddFontFromFileTTF(path, sizePixels, fontCfg, glyphRanges);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        font = nullptr;
    }
    return font;
}

static ImFont* RT_AddFontWithArialFallback(ImFontAtlas* atlas, const std::string& requestedPath, float sizePixels, const char* what,
                                          std::string* outUsedPath = nullptr) {
    if (!atlas) return nullptr;

    auto setUsed = [&](const std::string& p) {
        if (outUsedPath) { *outUsedPath = p; }
    };

    if (!requestedPath.empty() && RT_IsFontStable(requestedPath, sizePixels)) {
        if (ImFont* f = RT_SafeAddFontFromFileTTF(atlas, requestedPath.c_str(), sizePixels)) {
            setUsed(requestedPath);
            return f;
        }
    }

    const std::string& arial = ConfigDefaults::CONFIG_FONT_PATH;
    if (RT_IsFontStable(arial, sizePixels)) {
        Log(std::string("Render Thread: Falling back to Arial for ") + what);
        if (ImFont* f = RT_SafeAddFontFromFileTTF(atlas, arial.c_str(), sizePixels)) {
            setUsed(arial);
            return f;
        }
    }

    Log(std::string("Render Thread: Failed to load ") + what + ", using ImGui default font");
    setUsed(std::string());
    return atlas->AddFontDefault();
}

static bool RT_TryInitializeImGui(HWND hwnd, const Config& cfg) {
    if (g_renderThreadImGuiInitialized) { return true; }
    if (!hwnd) { return false; }

    IMGUI_CHECKVERSION();

    if (!g_renderThreadImGuiContext) {
        g_renderThreadImGuiContext = ImGui::CreateContext();
        if (!g_renderThreadImGuiContext) {
            Log("Render Thread: Failed to create ImGui context");
            return false;
        }
    }

    ImGui::SetCurrentContext(g_renderThreadImGuiContext);

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    const int screenHeight = GetCachedWindowHeight();
    float scaleFactor = 1.0f;
    if (screenHeight > 1080) { scaleFactor = static_cast<float>(screenHeight) / 1080.0f; }
    scaleFactor = roundf(scaleFactor * 4.0f) / 4.0f;
    if (scaleFactor < 1.0f) { scaleFactor = 1.0f; }
    g_eyeZoomScaleFactor = scaleFactor;

    {
        const std::string requestedBase = cfg.fontPath;
        (void)RT_AddFontWithArialFallback(io.Fonts, requestedBase, 16.0f * scaleFactor, "base font");
    }

    {
        std::string eyeZoomFontPath = cfg.eyezoom.textFontPath.empty() ? cfg.fontPath : cfg.eyezoom.textFontPath;
        g_eyeZoomTextFont = RT_AddFontWithArialFallback(io.Fonts, eyeZoomFontPath, 80.0f * scaleFactor, "EyeZoom font", &g_eyeZoomFontPathCached);
        if (g_eyeZoomFontPathCached.empty()) { g_eyeZoomFontPathCached = ConfigDefaults::CONFIG_FONT_PATH; }
    }

    ImGui::StyleColorsDark();
    LoadTheme();
    ApplyAppearanceConfig();
    ImGui::GetStyle().ScaleAllSizes(scaleFactor);

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplOpenGL3_Init("#version 330");

    InitializeOverlayTextFont(cfg.fontPath, 16.0f, scaleFactor);

    if (!io.Fonts->Build()) {
        Log("Render Thread: Initial font atlas build failed; falling back to ImGui default font");
        io.Fonts->Clear();
        io.Fonts->AddFontDefault();
        (void)io.Fonts->Build();
    }
    ImGui_ImplOpenGL3_DestroyFontsTexture();
    ImGui_ImplOpenGL3_CreateFontsTexture();
    if ((GLuint)(intptr_t)io.Fonts->TexID == 0) {
        Log("ERROR: Render Thread: ImGui font texture ID is 0 after initialization; GUI may render black");
    }

    g_fontsValid = true;
    g_renderThreadImGuiInitialized = true;
    LogCategory("init", "Render Thread: ImGui initialized successfully");
    return true;
}

// RENDER THREAD SHADER PROGRAMS
// These shaders are created on the render thread context (not shared with main thread)

static const char* rt_solid_vert_shader = R"(#version 330 core
layout(location = 0) in vec2 aPos;
void main() {
    gl_Position = vec4(aPos.x, aPos.y, 0.0, 1.0);
})";

static const char* rt_passthrough_vert_shader = R"(#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;
out vec2 TexCoord;
void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    TexCoord = aTexCoord;
})";

static const char* rt_background_frag_shader = R"(#version 330 core
out vec4 FragColor;
in vec2 TexCoord;
uniform sampler2D backgroundTexture;
uniform float u_opacity;
void main() {
    vec4 texColor = texture(backgroundTexture, TexCoord);
    FragColor = vec4(texColor.rgb, texColor.a * u_opacity);
})";

static const char* rt_solid_color_frag_shader = R"(#version 330 core
out vec4 FragColor;
uniform vec4 u_color;
void main() {
    FragColor = u_color;
})";

static const char* rt_image_render_frag_shader = R"(#version 330 core
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

static const char* rt_static_border_frag_shader = R"(#version 330 core
out vec4 FragColor;
in vec2 TexCoord;
uniform int u_shape;
uniform vec4 u_borderColor;
uniform float u_thickness;
uniform float u_radius;
uniform vec2 u_size;
uniform vec2 u_quadSize;

float sdRoundedBox(vec2 p, vec2 b, float r) {
    float maxR = min(b.x, b.y);
    r = clamp(r, 0.0, maxR);
    vec2 q = abs(p) - b + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

float sdEllipse(vec2 p, vec2 ab) {
    vec2 pn = p / ab;
    float len = length(pn);
    if (len < 0.0001) return -min(ab.x, ab.y);
    
    float d = len - 1.0;
    
    vec2 grad = pn / (ab * len);
    float gradLen = length(grad);
    
    return d / gradLen;
}

void main() {
    vec2 pixelPos = TexCoord * u_quadSize;
    
    vec2 centeredPixelPos = pixelPos - u_quadSize * 0.5;
    
    vec2 halfSize = max(u_size * 0.5, vec2(1.0, 1.0));
    
    float dist;
    
    if (u_shape == 0) {
        dist = sdRoundedBox(centeredPixelPos, halfSize, u_radius);
    } else {
        dist = sdEllipse(centeredPixelPos, halfSize);
    }
    
    float innerEdge = 0.0;
    float outerEdge = u_thickness;
    
    float epsilon = 0.5;
    
    if (dist >= innerEdge - epsilon && dist <= outerEdge + epsilon) {
        FragColor = u_borderColor;
    } else {
        discard;
    }
})";

static const char* rt_gradient_frag_shader = R"(#version 330 core
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

// NOTE: Border rendering shaders (brute force and JFA) have been removed from render_thread.
// All border rendering is now done by mirror_thread.cpp which has its own local shader programs.
// Render thread just blits the pre-rendered finalTexture using the passthrough/background shader.

// Optimized NV12 compute shader: writes Y plane as r8ui image (no atomics)
// UV plane is written to a separate r8ui image by even-coordinate threads only
static const char* rt_nv12_compute_shader = R"(
#version 430
layout(local_size_x = 16, local_size_y = 16) in;

uniform sampler2D u_rgbaTexture;
uniform uint u_width;
uniform uint u_height;

layout(r8ui, binding = 0) uniform writeonly uimage2D u_yPlane;
layout(r8ui, binding = 1) uniform writeonly uimage2D u_uvPlane;

void main() {
    uvec2 pos = gl_GlobalInvocationID.xy;
    if (pos.x >= u_width || pos.y >= u_height) return;

    uint srcY = u_height - 1u - pos.y;
    vec4 rgba = texelFetch(u_rgbaTexture, ivec2(pos.x, srcY), 0);

    float Y = 0.1826 * rgba.r + 0.6142 * rgba.g + 0.0620 * rgba.b + 0.0625;
    imageStore(u_yPlane, ivec2(pos.x, pos.y), uvec4(uint(clamp(Y * 255.0, 0.0, 255.0)), 0u, 0u, 0u));

    // UV plane: only even-coordinate threads (2x2 subsampling)
    if ((pos.x & 1u) == 0u && (pos.y & 1u) == 0u) {
        // Average 2x2 block for chroma
        vec4 p10 = texelFetch(u_rgbaTexture, ivec2(pos.x + 1u, srcY), 0);
        vec4 p01 = texelFetch(u_rgbaTexture, ivec2(pos.x, srcY - 1u), 0);
        vec4 p11 = texelFetch(u_rgbaTexture, ivec2(pos.x + 1u, srcY - 1u), 0);
        vec4 avg = (rgba + p10 + p01 + p11) * 0.25;

        float U = -0.1006 * avg.r - 0.3386 * avg.g + 0.4392 * avg.b + 0.5;
        float V =  0.4392 * avg.r - 0.3989 * avg.g - 0.0403 * avg.b + 0.5;

        uint uvRow = pos.y >> 1u;
        imageStore(u_uvPlane, ivec2(pos.x, uvRow), uvec4(uint(clamp(U * 255.0, 0.0, 255.0)), 0u, 0u, 0u));
        imageStore(u_uvPlane, ivec2(pos.x + 1u, uvRow), uvec4(uint(clamp(V * 255.0, 0.0, 255.0)), 0u, 0u, 0u));
    }
}
)";

static GLuint rt_backgroundProgram = 0;
static GLuint rt_solidColorProgram = 0;
static GLuint rt_imageRenderProgram = 0;
static GLuint rt_staticBorderProgram = 0;
static GLuint rt_gradientProgram = 0;

struct RT_BackgroundShaderLocs {
    GLint backgroundTexture = -1;
    GLint opacity = -1;
};

struct RT_SolidColorShaderLocs {
    GLint color = -1;
};

struct RT_ImageRenderShaderLocs {
    GLint imageTexture = -1;
    GLint enableColorKey = -1;
    GLint colorKey = -1;
    GLint sensitivity = -1;
    GLint opacity = -1;
};

struct RT_StaticBorderShaderLocs {
    GLint shape = -1, borderColor = -1, thickness = -1, radius = -1, size = -1, quadSize = -1;
};

struct RT_GradientShaderLocs {
    GLint numStops = -1;
    GLint stopColors = -1;
    GLint stopPositions = -1;
    GLint angle = -1;
    GLint time = -1;
    GLint animationType = -1;
    GLint animationSpeed = -1;
    GLint colorFade = -1;
};

static RT_BackgroundShaderLocs rt_backgroundShaderLocs;
static RT_SolidColorShaderLocs rt_solidColorShaderLocs;
static RT_ImageRenderShaderLocs rt_imageRenderShaderLocs;
static RT_StaticBorderShaderLocs rt_staticBorderShaderLocs;
static RT_GradientShaderLocs rt_gradientShaderLocs;

static GLuint RT_CompileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    GLint ok;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(shader, 512, NULL, log);
        Log("RenderThread: Shader compile failed: " + std::string(log));
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint RT_CreateShaderProgram(const char* vert, const char* frag) {
    GLuint v = RT_CompileShader(GL_VERTEX_SHADER, vert);
    GLuint f = RT_CompileShader(GL_FRAGMENT_SHADER, frag);
    if (v == 0 || f == 0) return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    GLint ok;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(p, 512, NULL, log);
        Log("RenderThread: Shader link failed: " + std::string(log));
        glDeleteProgram(p);
        p = 0;
    }
    glDeleteShader(v);
    glDeleteShader(f);
    return p;
}

static GLuint RT_CreateComputeProgram(const char* src) {
    GLuint cs = RT_CompileShader(GL_COMPUTE_SHADER, src);
    if (cs == 0) return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, cs);
    glLinkProgram(p);
    GLint ok;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(p, 512, NULL, log);
        Log("RenderThread: Compute shader link failed: " + std::string(log));
        glDeleteProgram(p);
        p = 0;
    }
    glDeleteShader(cs);
    return p;
}

static bool RT_InitializeShaders() {
    LogCategory("init", "RenderThread: Initializing shaders...");

    // NOTE: Border rendering shaders have been removed - all border rendering is done by mirror_thread
    // Render thread only needs: background (for mirror blitting), solid color (for game borders), image render, static border, and gradient
    rt_backgroundProgram = RT_CreateShaderProgram(rt_passthrough_vert_shader, rt_background_frag_shader);
    rt_solidColorProgram = RT_CreateShaderProgram(rt_solid_vert_shader, rt_solid_color_frag_shader);
    rt_imageRenderProgram = RT_CreateShaderProgram(rt_passthrough_vert_shader, rt_image_render_frag_shader);
    rt_staticBorderProgram = RT_CreateShaderProgram(rt_passthrough_vert_shader, rt_static_border_frag_shader);
    rt_gradientProgram = RT_CreateShaderProgram(rt_passthrough_vert_shader, rt_gradient_frag_shader);

    if (!rt_backgroundProgram || !rt_solidColorProgram || !rt_imageRenderProgram || !rt_staticBorderProgram || !rt_gradientProgram) {
        Log("RenderThread: FATAL - Failed to create shader programs");
        return false;
    }

    if (GLEW_ARB_compute_shader && GLEW_ARB_shader_image_load_store) {
        g_vcComputeProgram = RT_CreateComputeProgram(rt_nv12_compute_shader);
        if (g_vcComputeProgram) {
            g_vcUseCompute = true;
            g_vcLocRgbaTexture = glGetUniformLocation(g_vcComputeProgram, "u_rgbaTexture");
            g_vcLocWidth = glGetUniformLocation(g_vcComputeProgram, "u_width");
            g_vcLocHeight = glGetUniformLocation(g_vcComputeProgram, "u_height");
            LogCategory("init", "RenderThread: NV12 compute shader compiled successfully (Rec. 709, image2D path)");
        } else {
            Log("RenderThread: NV12 compute shader failed, falling back to CPU conversion");
            g_vcUseCompute = false;
        }
    } else {
        Log("RenderThread: Compute shaders not supported, using CPU NV12 conversion");
        g_vcUseCompute = false;
    }

    rt_backgroundShaderLocs.backgroundTexture = glGetUniformLocation(rt_backgroundProgram, "backgroundTexture");
    rt_backgroundShaderLocs.opacity = glGetUniformLocation(rt_backgroundProgram, "u_opacity");

    rt_solidColorShaderLocs.color = glGetUniformLocation(rt_solidColorProgram, "u_color");

    rt_staticBorderShaderLocs.shape = glGetUniformLocation(rt_staticBorderProgram, "u_shape");
    rt_staticBorderShaderLocs.borderColor = glGetUniformLocation(rt_staticBorderProgram, "u_borderColor");
    rt_staticBorderShaderLocs.thickness = glGetUniformLocation(rt_staticBorderProgram, "u_thickness");
    rt_staticBorderShaderLocs.radius = glGetUniformLocation(rt_staticBorderProgram, "u_radius");
    rt_staticBorderShaderLocs.size = glGetUniformLocation(rt_staticBorderProgram, "u_size");
    rt_staticBorderShaderLocs.quadSize = glGetUniformLocation(rt_staticBorderProgram, "u_quadSize");

    rt_imageRenderShaderLocs.imageTexture = glGetUniformLocation(rt_imageRenderProgram, "imageTexture");
    rt_imageRenderShaderLocs.enableColorKey = glGetUniformLocation(rt_imageRenderProgram, "u_enableColorKey");
    rt_imageRenderShaderLocs.colorKey = glGetUniformLocation(rt_imageRenderProgram, "u_colorKey");
    rt_imageRenderShaderLocs.sensitivity = glGetUniformLocation(rt_imageRenderProgram, "u_sensitivity");
    rt_imageRenderShaderLocs.opacity = glGetUniformLocation(rt_imageRenderProgram, "u_opacity");

    rt_gradientShaderLocs.numStops = glGetUniformLocation(rt_gradientProgram, "u_numStops");
    rt_gradientShaderLocs.stopColors = glGetUniformLocation(rt_gradientProgram, "u_stopColors");
    rt_gradientShaderLocs.stopPositions = glGetUniformLocation(rt_gradientProgram, "u_stopPositions");
    rt_gradientShaderLocs.angle = glGetUniformLocation(rt_gradientProgram, "u_angle");
    rt_gradientShaderLocs.time = glGetUniformLocation(rt_gradientProgram, "u_time");
    rt_gradientShaderLocs.animationType = glGetUniformLocation(rt_gradientProgram, "u_animationType");
    rt_gradientShaderLocs.animationSpeed = glGetUniformLocation(rt_gradientProgram, "u_animationSpeed");
    rt_gradientShaderLocs.colorFade = glGetUniformLocation(rt_gradientProgram, "u_colorFade");

    glUseProgram(rt_backgroundProgram);
    glUniform1i(rt_backgroundShaderLocs.backgroundTexture, 0);
    glUniform1f(rt_backgroundShaderLocs.opacity, 1.0f);

    glUseProgram(rt_imageRenderProgram);
    glUniform1i(rt_imageRenderShaderLocs.imageTexture, 0);

    glUseProgram(0);

    LogCategory("init", "RenderThread: Shaders initialized successfully");
    return true;
}

static void RT_CleanupShaders() {
    if (rt_backgroundProgram) {
        glDeleteProgram(rt_backgroundProgram);
        rt_backgroundProgram = 0;
    }
    if (rt_solidColorProgram) {
        glDeleteProgram(rt_solidColorProgram);
        rt_solidColorProgram = 0;
    }
    if (rt_imageRenderProgram) {
        glDeleteProgram(rt_imageRenderProgram);
        rt_imageRenderProgram = 0;
    }
    if (rt_gradientProgram) {
        glDeleteProgram(rt_gradientProgram);
        rt_gradientProgram = 0;
    }
}

static void RT_RenderCursorForObs(int fullW, int fullH, int viewportX, int viewportY, int viewportW, int viewportH, int windowW,
                                  int windowH, GLuint vao, GLuint vbo) {
    if (!IsCursorVisible()) { return; }

    CURSORINFO cursorInfo = { 0 };
    cursorInfo.cbSize = sizeof(CURSORINFO);
    if (!GetCursorInfo(&cursorInfo)) { return; }
    if (!cursorInfo.hCursor) { return; }
    if (!(cursorInfo.flags & CURSOR_SHOWING)) { return; }

    const CursorTextures::CursorData* cursorData = CursorTextures::LoadOrFindCursorFromHandle(cursorInfo.hCursor);
    if (!cursorData || cursorData->texture == 0) { return; }

    POINT cursorPos = cursorInfo.ptScreenPos;

    HWND hwnd = g_minecraftHwnd.load();
    if (hwnd) { ScreenToClient(hwnd, &cursorPos); }

    if (windowW > 0 && windowH > 0) {
        if (cursorPos.x < 0 || cursorPos.x >= windowW || cursorPos.y < 0 || cursorPos.y >= windowH) { return; }
    }

    float scaleX = (viewportW > 0 && windowW > 0) ? static_cast<float>(viewportW) / windowW : 1.0f;
    float scaleY = (viewportH > 0 && windowH > 0) ? static_cast<float>(viewportH) / windowH : 1.0f;

    int renderX = viewportX + static_cast<int>((cursorPos.x - cursorData->hotspotX) * scaleX);
    int renderY = viewportY + static_cast<int>((cursorPos.y - cursorData->hotspotY) * scaleY);

    int renderW = static_cast<int>(cursorData->bitmapWidth * scaleX);
    int renderH = static_cast<int>(cursorData->bitmapHeight * scaleY);

    if (renderW < 1) renderW = 1;
    if (renderH < 1) renderH = 1;

    if (renderX + renderW < 0 || renderX >= fullW || renderY + renderH < 0 || renderY >= fullH) { return; }

    glUseProgram(rt_imageRenderProgram);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, cursorData->texture);
    glUniform1i(rt_imageRenderShaderLocs.imageTexture, 0);
    glUniform1i(rt_imageRenderShaderLocs.enableColorKey, false);
    glUniform1f(rt_imageRenderShaderLocs.opacity, 1.0f);

    float left = (static_cast<float>(renderX) / fullW) * 2.0f - 1.0f;
    float right = (static_cast<float>(renderX + renderW) / fullW) * 2.0f - 1.0f;
    float top = 1.0f - (static_cast<float>(renderY) / fullH) * 2.0f;
    float bottom = 1.0f - (static_cast<float>(renderY + renderH) / fullH) * 2.0f;

    float cursorQuad[] = {
        left,  bottom, 0.0f, 1.0f,
        right, bottom, 1.0f, 1.0f,
        right, top,    1.0f, 0.0f,
        left,  bottom, 0.0f, 1.0f,
        right, top,    1.0f, 0.0f,
        left,  top,    0.0f, 0.0f
    };

    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(cursorQuad), cursorQuad);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    if (cursorData->hasInvertedPixels && cursorData->invertMaskTexture != 0) {
        glBindTexture(GL_TEXTURE_2D, cursorData->invertMaskTexture);
        glBlendFunc(GL_ONE_MINUS_DST_COLOR, GL_ONE_MINUS_SRC_ALPHA);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }

    glBindTexture(GL_TEXTURE_2D, 0);
}

// Render a border around an element using the render thread's shaders
// This mirrors RenderGameBorder() from render.cpp but uses render thread resources
static void RT_RenderGameBorder(int x, int y, int w, int h, int borderWidth, int radius, const Color& color, int fullW, int fullH,
                                GLuint vao, GLuint vbo) {
    if (borderWidth <= 0) return;

    glUseProgram(rt_solidColorProgram);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glUniform4f(rt_solidColorShaderLocs.color, color.r, color.g, color.b, 1.0f);

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

    float topBorder[] = { toNdcX(outerLeft),  toNdcY(y_gl + h), 0, 0, toNdcX(outerRight), toNdcY(y_gl + h), 0, 0,
                          toNdcX(outerRight), toNdcY(outerTop), 0, 0, toNdcX(outerLeft),  toNdcY(y_gl + h), 0, 0,
                          toNdcX(outerRight), toNdcY(outerTop), 0, 0, toNdcX(outerLeft),  toNdcY(outerTop), 0, 0 };
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(topBorder), topBorder);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    float bottomBorder[] = { toNdcX(outerLeft),  toNdcY(outerBottom), 0, 0, toNdcX(outerRight), toNdcY(outerBottom), 0, 0,
                             toNdcX(outerRight), toNdcY(y_gl),        0, 0, toNdcX(outerLeft),  toNdcY(outerBottom), 0, 0,
                             toNdcX(outerRight), toNdcY(y_gl),        0, 0, toNdcX(outerLeft),  toNdcY(y_gl),        0, 0 };
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(bottomBorder), bottomBorder);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    float leftBorder[] = { toNdcX(outerLeft), toNdcY(y_gl),     0, 0, toNdcX(x),         toNdcY(y_gl),     0, 0,
                           toNdcX(x),         toNdcY(y_gl + h), 0, 0, toNdcX(outerLeft), toNdcY(y_gl),     0, 0,
                           toNdcX(x),         toNdcY(y_gl + h), 0, 0, toNdcX(outerLeft), toNdcY(y_gl + h), 0, 0 };
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(leftBorder), leftBorder);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    float rightBorder[] = { toNdcX(x + w),      toNdcY(y_gl),     0, 0, toNdcX(outerRight), toNdcY(y_gl),     0, 0,
                            toNdcX(outerRight), toNdcY(y_gl + h), 0, 0, toNdcX(x + w),      toNdcY(y_gl),     0, 0,
                            toNdcX(outerRight), toNdcY(y_gl + h), 0, 0, toNdcX(x + w),      toNdcY(y_gl + h), 0, 0 };
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(rightBorder), rightBorder);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

static void RT_RenderBackground(bool isImage, GLuint bgTexture, float bgR, float bgG, float bgB, float opacity, int viewportX,
                                int viewportY, int viewportW, int viewportH, int letterboxExtendX, int letterboxExtendY, int fullW,
                                int fullH, GLuint vao, GLuint vbo) {
    if (viewportX == 0 && viewportY == 0 && viewportW == fullW && viewportH == fullH) return;

    int viewportY_gl = fullH - viewportY - viewportH;

    GLboolean scissorEnabled;
    glGetBooleanv(GL_SCISSOR_TEST, &scissorEnabled);

    glEnable(GL_STENCIL_TEST);
    glStencilMask(0xFF);
    glClear(GL_STENCIL_BUFFER_BIT);

    glStencilFunc(GL_ALWAYS, 1, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);

    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glUseProgram(rt_solidColorProgram);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);

    float vpNx1 = (static_cast<float>(viewportX + letterboxExtendX) / fullW) * 2.0f - 1.0f;
    float vpNx2 = (static_cast<float>(viewportX + viewportW - letterboxExtendX) / fullW) * 2.0f - 1.0f;
    float vpNy1 = (static_cast<float>(viewportY_gl + letterboxExtendY) / fullH) * 2.0f - 1.0f;
    float vpNy2 = (static_cast<float>(viewportY_gl + viewportH - letterboxExtendY) / fullH) * 2.0f - 1.0f;

    float stencilQuad[] = { vpNx1, vpNy1, 0, 0, vpNx2, vpNy1, 0, 0, vpNx2, vpNy2, 0, 0,
                            vpNx1, vpNy1, 0, 0, vpNx2, vpNy2, 0, 0, vpNx1, vpNy2, 0, 0 };
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(stencilQuad), stencilQuad);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glStencilMask(0x00);
    glStencilFunc(GL_EQUAL, 0, 0xFF);

    if (opacity < 1.0f) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    } else {
        glDisable(GL_BLEND);
    }

    if (isImage && bgTexture != 0) {
        glUseProgram(rt_backgroundProgram);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, bgTexture);
        glUniform1i(rt_backgroundShaderLocs.backgroundTexture, 0);
        glUniform1f(rt_backgroundShaderLocs.opacity, opacity);
    } else {
        glUseProgram(rt_solidColorProgram);
        glUniform4f(rt_solidColorShaderLocs.color, bgR, bgG, bgB, opacity);
    }

    float fullscreenQuad[] = { -1.0f, -1.0f, 0.0f, 0.0f, 1.0f, -1.0f, 1.0f, 0.0f, 1.0f,  1.0f, 1.0f, 1.0f,
                               -1.0f, -1.0f, 0.0f, 0.0f, 1.0f, 1.0f,  1.0f, 1.0f, -1.0f, 1.0f, 0.0f, 1.0f };
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(fullscreenQuad), fullscreenQuad);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    glDisable(GL_STENCIL_TEST);
    glStencilMask(0xFF);

    if (scissorEnabled) {
        glEnable(GL_SCISSOR_TEST);
    } else {
        glDisable(GL_SCISSOR_TEST);
    }
}

static void InitRenderFBOs(int width, int height) {
    bool mainResized = false;
    bool obsResized = false;

    for (int i = 0; i < RENDER_THREAD_FBO_COUNT; i++) {
        RenderFBO& fbo = g_renderFBOs[i];

        if (fbo.fbo == 0) { glGenFramebuffers(1, &fbo.fbo); }

        if (fbo.texture == 0) { glGenTextures(1, &fbo.texture); }

        if (fbo.stencilRbo == 0) { glGenRenderbuffers(1, &fbo.stencilRbo); }

        if (fbo.width != width || fbo.height != height) {
            glBindTexture(GL_TEXTURE_2D, fbo.texture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

            // One-time parameters used by the main-thread composite path.
            // Setting these here avoids per-frame glTexParameteri churn (driver overhead).
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_RED);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_GREEN);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_BLUE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_ALPHA);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);

            glBindRenderbuffer(GL_RENDERBUFFER, fbo.stencilRbo);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_STENCIL_INDEX8, width, height);

            glBindFramebuffer(GL_FRAMEBUFFER, fbo.fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fbo.texture, 0);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, fbo.stencilRbo);

            GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            if (status != GL_FRAMEBUFFER_COMPLETE) {
                Log("RenderThread: FBO " + std::to_string(i) + " incomplete: " + std::to_string(status));
            }

            fbo.width = width;
            fbo.height = height;
            mainResized = true;
            LogCategory("init", "RenderThread: Initialized FBO " + std::to_string(i) + " at " + std::to_string(width) + "x" +
                                    std::to_string(height));
        }

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    // (which uses the same GL name) now points to undefined content. The main thread
    // reading this would display garbage/frozen frames. Clear it so the main thread
    if (mainResized) {
        g_lastGoodTexture.store(0, std::memory_order_release);
        // Clear the fence pointer so the main thread doesn't wait on a stale fence.
        // Don't delete the old fence - it's managed by the deferred deletion ring buffer.
        (void)g_lastGoodFence.exchange(nullptr, std::memory_order_acq_rel);
    }

    for (int i = 0; i < RENDER_THREAD_FBO_COUNT; i++) {
        RenderFBO& fbo = g_obsRenderFBOs[i];

        if (fbo.fbo == 0) { glGenFramebuffers(1, &fbo.fbo); }

        if (fbo.texture == 0) { glGenTextures(1, &fbo.texture); }

        if (fbo.stencilRbo == 0) { glGenRenderbuffers(1, &fbo.stencilRbo); }

        if (fbo.width != width || fbo.height != height) {
            obsResized = true;
            glBindTexture(GL_TEXTURE_2D, fbo.texture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_RED);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_GREEN);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_BLUE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_ALPHA);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);

            glBindRenderbuffer(GL_RENDERBUFFER, fbo.stencilRbo);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_STENCIL_INDEX8, width, height);

            glBindFramebuffer(GL_FRAMEBUFFER, fbo.fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fbo.texture, 0);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, fbo.stencilRbo);

            GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            if (status != GL_FRAMEBUFFER_COMPLETE) {
                Log("RenderThread: OBS FBO " + std::to_string(i) + " incomplete: " + std::to_string(status));
            }

            fbo.width = width;
            fbo.height = height;
            LogCategory("init", "RenderThread: Initialized OBS FBO " + std::to_string(i) + " at " + std::to_string(width) + "x" +
                                    std::to_string(height));
        }

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    if (obsResized) {
        g_lastGoodObsTexture.store(0, std::memory_order_release);
        (void)g_lastGoodObsFence.exchange(nullptr, std::memory_order_acq_rel);
    }

    if (mainResized || obsResized) {
        glFlush();
    }
}

static void CleanupRenderFBOs() {
    for (int i = 0; i < RENDER_THREAD_FBO_COUNT; i++) {
        RenderFBO& fbo = g_renderFBOs[i];
        if (fbo.fbo != 0) {
            glDeleteFramebuffers(1, &fbo.fbo);
            fbo.fbo = 0;
        }
        if (fbo.texture != 0) {
            glDeleteTextures(1, &fbo.texture);
            fbo.texture = 0;
        }
        if (fbo.stencilRbo != 0) {
            glDeleteRenderbuffers(1, &fbo.stencilRbo);
            fbo.stencilRbo = 0;
        }
        if (fbo.gpuFence != nullptr) {
            if (glIsSync(fbo.gpuFence)) { glDeleteSync(fbo.gpuFence); }
            fbo.gpuFence = nullptr;
        }
        fbo.width = 0;
        fbo.height = 0;
        fbo.ready.store(false);
    }

    for (int i = 0; i < RENDER_THREAD_FBO_COUNT; i++) {
        RenderFBO& fbo = g_obsRenderFBOs[i];
        if (fbo.fbo != 0) {
            glDeleteFramebuffers(1, &fbo.fbo);
            fbo.fbo = 0;
        }
        if (fbo.texture != 0) {
            glDeleteTextures(1, &fbo.texture);
            fbo.texture = 0;
        }
        if (fbo.stencilRbo != 0) {
            glDeleteRenderbuffers(1, &fbo.stencilRbo);
            fbo.stencilRbo = 0;
        }
        if (fbo.gpuFence != nullptr) {
            if (glIsSync(fbo.gpuFence)) { glDeleteSync(fbo.gpuFence); }
            fbo.gpuFence = nullptr;
        }
        fbo.width = 0;
        fbo.height = 0;
        fbo.ready.store(false);
    }

    if (g_virtualCamPBO != 0) {
        glDeleteBuffers(1, &g_virtualCamPBO);
        g_virtualCamPBO = 0;
    }
    if (g_virtualCamCopyFBO != 0) {
        glDeleteFramebuffers(1, &g_virtualCamCopyFBO);
        g_virtualCamCopyFBO = 0;
    }
    g_virtualCamPBOWidth = 0;
    g_virtualCamPBOHeight = 0;
    g_virtualCamPBOPending = false;

    for (int i = 0; i < 2; i++) {
        if (g_vcYImage[i] != 0) {
            glDeleteTextures(1, &g_vcYImage[i]);
            g_vcYImage[i] = 0;
        }
        if (g_vcUVImage[i] != 0) {
            glDeleteTextures(1, &g_vcUVImage[i]);
            g_vcUVImage[i] = 0;
        }
        if (g_vcReadbackPBO[i] != 0) {
            glDeleteBuffers(1, &g_vcReadbackPBO[i]);
            g_vcReadbackPBO[i] = 0;
        }
    }
    if (g_vcReadbackFBO != 0) {
        glDeleteFramebuffers(1, &g_vcReadbackFBO);
        g_vcReadbackFBO = 0;
    }
    if (g_vcFence) {
        if (glIsSync(g_vcFence)) { glDeleteSync(g_vcFence); }
        g_vcFence = nullptr;
    }
    if (g_vcScaleFBO != 0) {
        glDeleteFramebuffers(1, &g_vcScaleFBO);
        g_vcScaleFBO = 0;
    }
    if (g_vcScaleTexture != 0) {
        glDeleteTextures(1, &g_vcScaleTexture);
        g_vcScaleTexture = 0;
    }
    g_vcOutWidth = 0;
    g_vcOutHeight = 0;
    g_vcComputePending = false;
    g_vcReadbackPending = false;

    if (g_vcCursorFBO != 0) {
        glDeleteFramebuffers(1, &g_vcCursorFBO);
        g_vcCursorFBO = 0;
    }
    if (g_vcCursorTexture != 0) {
        glDeleteTextures(1, &g_vcCursorTexture);
        g_vcCursorTexture = 0;
    }
    g_vcCursorWidth = 0;
    g_vcCursorHeight = 0;
}

static void AdvanceWriteFBO() {
    int current = g_writeFBOIndex.load();
    int next = (current + 1) % RENDER_THREAD_FBO_COUNT;

    g_renderFBOs[current].ready.store(true, std::memory_order_release);
    g_readFBOIndex.store(current, std::memory_order_release);

    g_writeFBOIndex.store(next);

    g_renderFBOs[next].ready.store(false, std::memory_order_release);
}

static void AdvanceObsFBO() {
    int current = g_obsWriteFBOIndex.load();
    int next = (current + 1) % RENDER_THREAD_FBO_COUNT;

    g_obsRenderFBOs[current].ready.store(true, std::memory_order_release);
    g_obsReadFBOIndex.store(current, std::memory_order_release);
    g_obsWriteFBOIndex.store(next);
    g_obsRenderFBOs[next].ready.store(false, std::memory_order_release);
}

static void GetVirtualCamScaledSize(int srcW, int srcH, float scale, int& outW, int& outH) {
    outW = static_cast<int>(srcW * scale);
    outH = static_cast<int>(srcH * scale);
    outW = (outW + 1) & ~1;
    outH = (outH + 1) & ~1;
    if (outW < 64) outW = 64;
    if (outH < 64) outH = 64;
}

static void EnsureVCScaleResources(int w, int h) {
    if (g_vcScaleWidth == w && g_vcScaleHeight == h && g_vcScaleFBO != 0) return;

    if (g_vcScaleFBO == 0) glGenFramebuffers(1, &g_vcScaleFBO);
    if (g_vcScaleTexture != 0) glDeleteTextures(1, &g_vcScaleTexture);
    glGenTextures(1, &g_vcScaleTexture);
    glBindTexture(GL_TEXTURE_2D, g_vcScaleTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    glBindFramebuffer(GL_FRAMEBUFFER, g_vcScaleFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_vcScaleTexture, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    g_vcScaleWidth = w;
    g_vcScaleHeight = h;
}

static void EnsureVCImageResources(int w, int h) {
    if (g_vcOutWidth == w && g_vcOutHeight == h && g_vcYImage[0] != 0) return;

    uint32_t nv12Size = w * h * 3 / 2;

    for (int i = 0; i < 2; i++) {
        if (g_vcYImage[i] != 0) glDeleteTextures(1, &g_vcYImage[i]);
        glGenTextures(1, &g_vcYImage[i]);
        glBindTexture(GL_TEXTURE_2D, g_vcYImage[i]);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_R8UI, w, h);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindTexture(GL_TEXTURE_2D, 0);

        if (g_vcUVImage[i] != 0) glDeleteTextures(1, &g_vcUVImage[i]);
        glGenTextures(1, &g_vcUVImage[i]);
        glBindTexture(GL_TEXTURE_2D, g_vcUVImage[i]);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_R8UI, w, h / 2);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindTexture(GL_TEXTURE_2D, 0);

        if (g_vcReadbackPBO[i] != 0) glDeleteBuffers(1, &g_vcReadbackPBO[i]);
        glGenBuffers(1, &g_vcReadbackPBO[i]);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, g_vcReadbackPBO[i]);
        glBufferData(GL_PIXEL_PACK_BUFFER, nv12Size, nullptr, GL_STREAM_READ);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    }

    if (g_vcReadbackFBO == 0) glGenFramebuffers(1, &g_vcReadbackFBO);

    g_vcOutWidth = w;
    g_vcOutHeight = h;
    g_vcWriteIdx = 0;
    g_vcComputePending = false;
    g_vcReadbackPending = false;
    if (g_vcFence) {
        if (glIsSync(g_vcFence)) { glDeleteSync(g_vcFence); }
        g_vcFence = nullptr;
    }
}

static void FlushVirtualCameraReadback() {
    if (!g_vcReadbackPending) return;

    int readIdx = 1 - g_vcWriteIdx;
    glBindBuffer(GL_PIXEL_PACK_BUFFER, g_vcReadbackPBO[readIdx]);
    void* data = glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
    if (data) {
        LARGE_INTEGER counter, freq;
        QueryPerformanceCounter(&counter);
        QueryPerformanceFrequency(&freq);
        uint64_t timestamp = (counter.QuadPart * 10000000ULL) / freq.QuadPart;
        WriteVirtualCameraFrameNV12(static_cast<const uint8_t*>(data), g_vcOutWidth, g_vcOutHeight, timestamp);
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    g_vcReadbackPending = false;
}

static void StartVirtualCameraComputeReadback(GLuint srcTexture, int texW, int texH, int outW, int outH) {
    if (g_vcComputePending && g_vcFence) {
        // Non-blocking check: if GPU isn't done yet, skip this frame's virtual camera update
        GLenum result = glClientWaitSync(g_vcFence, 0, 0);
        if (result == GL_ALREADY_SIGNALED || result == GL_CONDITION_SATISFIED) {
            if (glIsSync(g_vcFence)) { glDeleteSync(g_vcFence); }
            g_vcFence = nullptr;
            g_vcComputePending = false;

            int readIdx = g_vcWriteIdx;
            uint32_t ySize = outW * outH;

            glBindBuffer(GL_PIXEL_PACK_BUFFER, g_vcReadbackPBO[readIdx]);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, g_vcReadbackFBO);

            glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_vcYImage[readIdx], 0);
            glReadPixels(0, 0, outW, outH, GL_RED_INTEGER, GL_UNSIGNED_BYTE, (void*)0);

            glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_vcUVImage[readIdx], 0);
            glReadPixels(0, 0, outW, outH / 2, GL_RED_INTEGER, GL_UNSIGNED_BYTE, (void*)(uintptr_t)ySize);

            glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

            g_vcReadbackPending = true;
        }
    }

    FlushVirtualCameraReadback();

    EnsureVCImageResources(outW, outH);

    g_vcWriteIdx = 1 - g_vcWriteIdx;
    int writeIdx = g_vcWriteIdx;

    GLuint sampleTexture = srcTexture;
    if (outW != texW || outH != texH) {
        EnsureVCScaleResources(outW, outH);
        if (g_virtualCamCopyFBO == 0) glGenFramebuffers(1, &g_virtualCamCopyFBO);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g_virtualCamCopyFBO);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, srcTexture, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_vcScaleFBO);
        glBlitFramebuffer(0, 0, texW, texH, 0, 0, outW, outH, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        sampleTexture = g_vcScaleTexture;
    }

    // Step 6: Dispatch compute shader with image2D bindings (no atomics, no SSBO clear)
    glUseProgram(g_vcComputeProgram);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sampleTexture);
    glUniform1i(g_vcLocRgbaTexture, 0);
    glUniform1ui(g_vcLocWidth, outW);
    glUniform1ui(g_vcLocHeight, outH);

    glBindImageTexture(0, g_vcYImage[writeIdx], 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R8UI);
    glBindImageTexture(1, g_vcUVImage[writeIdx], 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R8UI);

    GLuint groupsX = (outW + 15) / 16;
    GLuint groupsY = (outH + 15) / 16;
    glDispatchCompute(groupsX, groupsY, 1);

    // Fence after dispatch — we'll check it next frame (non-blocking)
    g_vcFence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();

    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);

    g_vcComputePending = true;
}

static void StartVirtualCameraPBOReadback(GLuint obsTexture, int width, int height) {
    if (g_virtualCamPBOPending && g_virtualCamPBO != 0) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, g_virtualCamPBO);
        void* data = glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
        if (data) {
            LARGE_INTEGER counter, freq;
            QueryPerformanceCounter(&counter);
            QueryPerformanceFrequency(&freq);
            uint64_t timestamp = (counter.QuadPart * 10000000ULL) / freq.QuadPart;

            WriteVirtualCameraFrame(static_cast<const uint8_t*>(data), g_virtualCamPBOWidth, g_virtualCamPBOHeight, timestamp);
            glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        g_virtualCamPBOPending = false;
    }

    if (g_virtualCamPBOWidth != width || g_virtualCamPBOHeight != height || g_virtualCamPBO == 0) {
        if (g_virtualCamPBO != 0) { glDeleteBuffers(1, &g_virtualCamPBO); }
        glGenBuffers(1, &g_virtualCamPBO);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, g_virtualCamPBO);
        glBufferData(GL_PIXEL_PACK_BUFFER, width * height * 4, nullptr, GL_STREAM_READ);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

        g_virtualCamPBOWidth = width;
        g_virtualCamPBOHeight = height;

        if (g_virtualCamCopyFBO == 0) { glGenFramebuffers(1, &g_virtualCamCopyFBO); }
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_virtualCamCopyFBO);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, obsTexture, 0);

    glBindBuffer(GL_PIXEL_PACK_BUFFER, g_virtualCamPBO);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

    g_virtualCamPBOPending = true;
}

static void StartVirtualCameraAsyncReadback(GLuint obsTexture, int width, int height) {
    if (obsTexture == 0 || width <= 0 || height <= 0) return;
    if (!IsVirtualCameraActive()) return;

    int outW, outH;
    GetVirtualCamScaledSize(width, height, 1.0f, outW, outH);

    if (g_vcUseCompute && g_vcComputeProgram != 0) {
        StartVirtualCameraComputeReadback(obsTexture, width, height, outW, outH);
    } else {
        StartVirtualCameraPBOReadback(obsTexture, width, height);
    }
}

static void RT_RenderGameTexture(GLuint gameTexture, int x, int y, int w, int h, int fullW, int fullH, int srcGameW, int srcGameH, int texW,
                                 int texH, GLuint vao, GLuint vbo) {
    if (gameTexture == UINT_MAX) return;

    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, gameTexture);

    glUseProgram(rt_backgroundProgram);
    glUniform1f(rt_backgroundShaderLocs.opacity, 1.0f);
    glDisable(GL_BLEND);

    int y_gl = fullH - y - h;
    float nx1 = (static_cast<float>(x) / fullW) * 2.0f - 1.0f;
    float ny1 = (static_cast<float>(y_gl) / fullH) * 2.0f - 1.0f;
    float nx2 = (static_cast<float>(x + w) / fullW) * 2.0f - 1.0f;
    float ny2 = (static_cast<float>(y_gl + h) / fullH) * 2.0f - 1.0f;

    float u_max = (texW > 0) ? static_cast<float>(srcGameW) / texW : 1.0f;
    float v_max = (texH > 0) ? static_cast<float>(srcGameH) / texH : 1.0f;

    float verts[] = {
        nx1, ny1, 0.0f,  0.0f,
        nx2, ny1, u_max, 0.0f,
        nx2, ny2, u_max, v_max,
        nx1, ny1, 0.0f,  0.0f,
        nx2, ny2, u_max, v_max,
        nx1, ny2, 0.0f,  v_max
    };

    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
}

// Render EyeZoom overlay on render thread for OBS capture
static void RT_RenderEyeZoom(GLuint gameTexture, int requestViewportX, int fullW, int fullH, int gameTexW, int gameTexH, GLuint vao,
                             GLuint vbo, bool isTransitioningFromEyeZoom = false, GLuint snapshotTexture = 0, int snapshotWidth = 0,
                             int snapshotHeight = 0, const EyeZoomConfig* externalZoomConfig = nullptr) {
    if (gameTexture == UINT_MAX) return;

    // Using the caller's snapshot avoids a TOCTOU race where the config changes between
    EyeZoomConfig zoomConfig;
    if (externalZoomConfig) {
        zoomConfig = *externalZoomConfig;
    } else {
        auto zoomCfgSnap = GetConfigSnapshot();
        if (!zoomCfgSnap) return;
        zoomConfig = zoomCfgSnap->eyezoom;
    }

    int modeWidth = zoomConfig.windowWidth;
    int targetViewportX = (fullW - modeWidth) / 2;

    int viewportX = (requestViewportX >= 0) ? requestViewportX : targetViewportX;
    bool isTransitioningToEyeZoom = (viewportX < targetViewportX && !isTransitioningFromEyeZoom);

    int zoomOutputWidth = 0;
    int zoomOutputHeight = 0;
    int finalZoomX = 0;
    int zoomY = 0;

    if (zoomConfig.useCustomSizePosition) {
        zoomOutputWidth = zoomConfig.zoomAreaWidth;
        zoomOutputHeight = zoomConfig.zoomAreaHeight;
        finalZoomX = zoomConfig.positionX;
        zoomY = zoomConfig.positionY;
    } else {
        int autoHorizontalMargin = 0;
        if (targetViewportX > 0) autoHorizontalMargin = targetViewportX / 10;

        zoomOutputWidth = targetViewportX - (2 * autoHorizontalMargin);
        int autoVerticalMargin = fullH / 8;
        zoomOutputHeight = fullH - (2 * autoVerticalMargin);

        finalZoomX = autoHorizontalMargin;
        zoomY = (fullH - zoomOutputHeight) / 2;
    }

    if (zoomOutputWidth > fullW) zoomOutputWidth = fullW;

    if (zoomOutputWidth <= 1) {
        return;
    }

    int zoomX = finalZoomX;

    if (zoomConfig.slideZoomIn) {
        int offScreenX = -zoomOutputWidth;

        if ((isTransitioningToEyeZoom || isTransitioningFromEyeZoom) && targetViewportX > 0) {
            float progress = (float)viewportX / (float)targetViewportX;
            zoomX = offScreenX + (int)((finalZoomX - offScreenX) * progress);
        }
    }

    if (zoomOutputHeight > fullH) zoomOutputHeight = fullH;
    if (zoomOutputHeight < 1) zoomOutputHeight = 1;

    int maxZoomX = (std::max)(0, fullW - zoomOutputWidth);
    int maxZoomY = (std::max)(0, fullH - zoomOutputHeight);

    bool isSlidingNow = zoomConfig.slideZoomIn && (isTransitioningToEyeZoom || isTransitioningFromEyeZoom);
    if (!isSlidingNow) {
        zoomX = (std::max)(0, (std::min)(zoomX, maxZoomX));
    }
    zoomY = (std::max)(0, (std::min)(zoomY, maxZoomY));

    int zoomY_gl = fullH - zoomY - zoomOutputHeight;

    GLint currentDrawFBO;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &currentDrawFBO);


    auto EnsureRtEyeZoomSnapshotAllocated = [&]() {
        if (rt_eyeZoomSnapshotTexture == 0 || rt_eyeZoomSnapshotWidth != zoomOutputWidth || rt_eyeZoomSnapshotHeight != zoomOutputHeight) {
            if (rt_eyeZoomSnapshotTexture != 0) { glDeleteTextures(1, &rt_eyeZoomSnapshotTexture); }
            if (rt_eyeZoomSnapshotFBO != 0) { glDeleteFramebuffers(1, &rt_eyeZoomSnapshotFBO); }

            glGenTextures(1, &rt_eyeZoomSnapshotTexture);
            glBindTexture(GL_TEXTURE_2D, rt_eyeZoomSnapshotTexture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, zoomOutputWidth, zoomOutputHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

            glGenFramebuffers(1, &rt_eyeZoomSnapshotFBO);
            glBindFramebuffer(GL_FRAMEBUFFER, rt_eyeZoomSnapshotFBO);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt_eyeZoomSnapshotTexture, 0);

            rt_eyeZoomSnapshotWidth = zoomOutputWidth;
            rt_eyeZoomSnapshotHeight = zoomOutputHeight;
            rt_eyeZoomSnapshotValid = false;
        }
    };

    auto BlitRtEyeZoomSnapshotToDest = [&]() {
        static GLuint snapshotReadFBO = 0;
        if (snapshotReadFBO == 0) { glGenFramebuffers(1, &snapshotReadFBO); }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, snapshotReadFBO);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt_eyeZoomSnapshotTexture, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, currentDrawFBO);
        glBlitFramebuffer(0, 0, rt_eyeZoomSnapshotWidth, rt_eyeZoomSnapshotHeight, zoomX, zoomY_gl, zoomX + zoomOutputWidth,
                          zoomY_gl + zoomOutputHeight, GL_COLOR_BUFFER_BIT, GL_NEAREST);
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

    if (isTransitioningFromEyeZoom && rt_eyeZoomSnapshotValid && rt_eyeZoomSnapshotTexture != 0) {
        BlitRtEyeZoomSnapshotToDest();
    } else {
        int texWidth = gameTexW;
        int texHeight = gameTexH;

        int srcCenterX = texWidth / 2;
        int srcLeft = srcCenterX - zoomConfig.cloneWidth / 2;
        int srcRight = srcCenterX + zoomConfig.cloneWidth / 2;

        int srcCenterY = texHeight / 2;
        int srcBottom = srcCenterY - zoomConfig.cloneHeight / 2;
        int srcTop = srcCenterY + zoomConfig.cloneHeight / 2;

        // Clamp to valid source region to avoid undefined blits.
        srcLeft = (std::max)(0, srcLeft);
        srcBottom = (std::max)(0, srcBottom);
        srcRight = (std::min)(texWidth, srcRight);
        srcTop = (std::min)(texHeight, srcTop);
        if (srcRight <= srcLeft || srcTop <= srcBottom) {
            return;
        }

        int dstLeft = zoomX;
        int dstRight = zoomX + zoomOutputWidth;
        int dstBottom = zoomY_gl;
        int dstTop = zoomY_gl + zoomOutputHeight;

        static GLuint gameReadFBO = 0;
        if (gameReadFBO == 0) { glGenFramebuffers(1, &gameReadFBO); }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, gameReadFBO);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gameTexture, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, currentDrawFBO);
        glBlitFramebuffer(srcLeft, srcBottom, srcRight, srcTop, dstLeft, dstBottom, dstRight, dstTop, GL_COLOR_BUFFER_BIT, GL_NEAREST);

        EnsureRtEyeZoomSnapshotAllocated();
        glBindFramebuffer(GL_READ_FRAMEBUFFER, currentDrawFBO);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, rt_eyeZoomSnapshotFBO);
        glBlitFramebuffer(dstLeft, dstBottom, dstRight, dstTop, 0, 0, zoomOutputWidth, zoomOutputHeight, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, currentDrawFBO);
        rt_eyeZoomSnapshotValid = true;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, currentDrawFBO);
    ForceOpaqueAlphaInCurrentDrawFbo(zoomX, zoomY_gl, zoomOutputWidth, zoomOutputHeight);

    glBindFramebuffer(GL_FRAMEBUFFER, currentDrawFBO);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(rt_solidColorProgram);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);

    bool useDefaultOverlay = (zoomConfig.activeOverlayIndex < 0 ||
                              zoomConfig.activeOverlayIndex >= (int)zoomConfig.overlays.size());

    if (useDefaultOverlay) {
        // Render default numbered boxes
        float pixelWidthOnScreen = zoomOutputWidth / (float)zoomConfig.cloneWidth;
        int labelsPerSide = zoomConfig.cloneWidth / 2;
        int overlayLabelsPerSide = zoomConfig.overlayWidth;
        if (overlayLabelsPerSide < 0) overlayLabelsPerSide = labelsPerSide;
        if (overlayLabelsPerSide > labelsPerSide) overlayLabelsPerSide = labelsPerSide;
        float centerY = zoomY_gl + zoomOutputHeight / 2.0f;

        float boxHeight = zoomConfig.linkRectToFont ? (zoomConfig.textFontSize * 1.2f) : (float)zoomConfig.rectHeight;

        for (int xOffset = -overlayLabelsPerSide; xOffset <= overlayLabelsPerSide; xOffset++) {
            if (xOffset == 0) continue;

            int boxIndex = xOffset + labelsPerSide - (xOffset > 0 ? 1 : 0);
            float boxLeft = zoomX + (boxIndex * pixelWidthOnScreen);
            float boxRight = boxLeft + pixelWidthOnScreen;
            float boxBottom = centerY - boxHeight / 2.0f;
            float boxTop = centerY + boxHeight / 2.0f;

            Color boxColor = (boxIndex % 2 == 0) ? zoomConfig.gridColor1 : zoomConfig.gridColor2;
            float boxOpacity = (boxIndex % 2 == 0) ? zoomConfig.gridColor1Opacity : zoomConfig.gridColor2Opacity;
            glUniform4f(rt_solidColorShaderLocs.color, boxColor.r, boxColor.g, boxColor.b, boxOpacity);

            float boxNdcLeft = (boxLeft / (float)fullW) * 2.0f - 1.0f;
            float boxNdcRight = (boxRight / (float)fullW) * 2.0f - 1.0f;
            float boxNdcBottom = (boxBottom / (float)fullH) * 2.0f - 1.0f;
            float boxNdcTop = (boxTop / (float)fullH) * 2.0f - 1.0f;

            float boxVerts[] = {
                boxNdcLeft, boxNdcBottom, 0, 0, boxNdcRight, boxNdcBottom, 0, 0, boxNdcRight, boxNdcTop, 0, 0,
                boxNdcLeft, boxNdcBottom, 0, 0, boxNdcRight, boxNdcTop,    0, 0, boxNdcLeft,  boxNdcTop, 0, 0,
            };
            glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(boxVerts), boxVerts);
            glDrawArrays(GL_TRIANGLES, 0, 6);

            int displayNumber = abs(xOffset);
            float numberCenterX = boxLeft + pixelWidthOnScreen / 2.0f;
            float numberCenterY = centerY;
            // For now, we'll skip text labels for OBS (they require cross-thread coordination)
        }
    }

    float centerX = zoomX + zoomOutputWidth / 2.0f;
    float centerLineWidth = 2.0f;
    float lineLeft = centerX - centerLineWidth / 2.0f;
    float lineRight = centerX + centerLineWidth / 2.0f;
    float lineBottom = (float)zoomY_gl;
    float lineTop = (float)(zoomY_gl + zoomOutputHeight);

    float lineNdcLeft = (lineLeft / (float)fullW) * 2.0f - 1.0f;
    float lineNdcRight = (lineRight / (float)fullW) * 2.0f - 1.0f;
    float lineNdcBottom = (lineBottom / (float)fullH) * 2.0f - 1.0f;
    float lineNdcTop = (lineTop / (float)fullH) * 2.0f - 1.0f;

    glUniform4f(rt_solidColorShaderLocs.color, zoomConfig.centerLineColor.r, zoomConfig.centerLineColor.g, zoomConfig.centerLineColor.b,
                zoomConfig.centerLineColorOpacity);

    float centerLineVerts[] = {
        lineNdcLeft, lineNdcBottom, 0, 0, lineNdcRight, lineNdcBottom, 0, 0, lineNdcRight, lineNdcTop, 0, 0,
        lineNdcLeft, lineNdcBottom, 0, 0, lineNdcRight, lineNdcTop,    0, 0, lineNdcLeft,  lineNdcTop, 0, 0,
    };
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(centerLineVerts), centerLineVerts);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    // Render the selected custom EyeZoom overlay image
    if (!useDefaultOverlay) {
        const auto& ov = zoomConfig.overlays[zoomConfig.activeOverlayIndex];
        glUseProgram(rt_imageRenderProgram);
        glUniform1i(rt_imageRenderShaderLocs.enableColorKey, 0);

        std::lock_guard<std::mutex> lock(g_userImagesMutex);
        std::string imageId = "ezoverlay_" + ov.name;
        auto it = g_userImages.find(imageId);
        if (it != g_userImages.end() && it->second.textureId != 0) {
            const UserImageInstance& inst = it->second;
            int texW = inst.width;
            int texH = inst.height;
            if (texW > 0 && texH > 0) {
                float displayW, displayH;
                switch (ov.displayMode) {
                    case EyeZoomOverlayDisplayMode::Manual:
                        displayW = (float)ov.manualWidth;
                        displayH = (float)ov.manualHeight;
                        break;
                    case EyeZoomOverlayDisplayMode::Stretch:
                        displayW = (float)zoomOutputWidth;
                        displayH = (float)zoomOutputHeight;
                        break;
                    case EyeZoomOverlayDisplayMode::Fit:
                    default: {
                        float scaleF = (std::min)((float)zoomOutputWidth / texW, (float)zoomOutputHeight / texH);
                        displayW = texW * scaleF;
                        displayH = texH * scaleF;
                        break;
                    }
                }

                float ovLeft = zoomX + (zoomOutputWidth - displayW) / 2.0f;
                float ovBottom_gl = zoomY_gl + (zoomOutputHeight - displayH) / 2.0f;
                float ovRight = ovLeft + displayW;
                float ovTop_gl = ovBottom_gl + displayH;

                float nx1 = (ovLeft / (float)fullW) * 2.0f - 1.0f;
                float nx2 = (ovRight / (float)fullW) * 2.0f - 1.0f;
                float ny1 = (ovBottom_gl / (float)fullH) * 2.0f - 1.0f;
                float ny2 = (ovTop_gl / (float)fullH) * 2.0f - 1.0f;

                glBindTexture(GL_TEXTURE_2D, inst.textureId);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glUniform1f(rt_imageRenderShaderLocs.opacity, ov.opacity);

                float ovVerts[] = {
                    nx1, ny1, 0.0f, 0.0f,
                    nx2, ny1, 1.0f, 0.0f,
                    nx2, ny2, 1.0f, 1.0f,
                    nx1, ny1, 0.0f, 0.0f,
                    nx2, ny2, 1.0f, 1.0f,
                    nx1, ny2, 0.0f, 1.0f,
                };
                glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(ovVerts), ovVerts);
                glDrawArrays(GL_TRIANGLES, 0, 6);
            }
        }
    }

    glDisable(GL_BLEND);
}

// Render mirrors using render thread's local shader programs
static void RT_RenderMirrors(const std::vector<MirrorConfig>& activeMirrors, const GameViewportGeometry& geo, int fullW, int fullH,
                             float modeOpacity, bool excludeOnlyOnMyScreen, bool relativeStretching, float transitionProgress,
                             float mirrorSlideProgress, int fromX, int fromY, int fromW, int fromH, int toX, int toY, int toW, int toH,
                             bool isEyeZoomMode, bool isTransitioningFromEyeZoom, int eyeZoomAnimatedViewportX, bool skipAnimation,
                             const std::string& fromModeId, bool fromSlideMirrorsIn, bool toSlideMirrorsIn, bool isSlideOutPass, GLuint vao,
                             GLuint vbo) {
    if (activeMirrors.empty()) return;

    // Grab config snapshot for thread-safe access
    auto slideCfgSnap = GetConfigSnapshot();
    if (!slideCfgSnap) return;
    const Config& slideCfg = *slideCfgSnap;

    std::set<std::string> sourceMirrorNames;
    if (!fromModeId.empty() && (fromSlideMirrorsIn || toSlideMirrorsIn || slideCfg.eyezoom.slideMirrorsIn)) {
        for (const auto& mode : slideCfg.modes) {
            if (EqualsIgnoreCase(mode.id, fromModeId)) {
                for (const auto& mirrorName : mode.mirrorIds) { sourceMirrorNames.insert(mirrorName); }
                for (const auto& groupName : mode.mirrorGroupIds) {
                    for (const auto& group : slideCfg.mirrorGroups) {
                        if (group.name == groupName) {
                            for (const auto& item : group.mirrors) { sourceMirrorNames.insert(item.mirrorId); }
                            break;
                        }
                    }
                }
                break;
            }
        }
    }

    // PHASE 1: Copy all needed data under the lock (fast, no GPU waits)
    // PHASE 2: Wait on GPU fences OUTSIDE the lock (avoids blocking mirror thread)
    std::vector<MirrorRenderData> mirrorsToRender;
    mirrorsToRender.reserve(activeMirrors.size());

    // Temporary struct to hold fence + index for deferred fence wait
    struct PendingFenceWait {
        GLsync fence;
        size_t renderDataIndex;
    };
    std::vector<PendingFenceWait> pendingFences;

    {
        // PHASE 1: Shared (read) lock - just copy data, no GPU operations
        std::shared_lock<std::shared_mutex> mirrorLock(g_mirrorInstancesMutex);
        for (const auto& conf : activeMirrors) {
            if (excludeOnlyOnMyScreen && conf.onlyOnMyScreen) continue;

            // If the mirror is fully transparent, skip EVERYTHING (including fence waits).
            const float effectiveOpacity = modeOpacity * conf.opacity;
            if (effectiveOpacity <= 0.0f) continue;

            auto it = g_mirrorInstances.find(conf.name);
            if (it == g_mirrorInstances.end()) continue;

            const MirrorInstance& inst = it->second;
            if (!inst.hasValidContent) continue;

            MirrorRenderData data;
            data.config = &conf;

            float scaleX = conf.output.separateScale ? conf.output.scaleX : conf.output.scale;
            float scaleY = conf.output.separateScale ? conf.output.scaleY : conf.output.scale;

            // ALWAYS prefer finalTexture when available - it has borders already applied by mirror_thread
            if (inst.finalTexture != 0 && inst.final_w > 0 && inst.final_h > 0) {
                data.texture = inst.finalTexture;
                data.tex_w = inst.final_w;
                data.tex_h = inst.final_h;
                data.outW = static_cast<int>(inst.fbo_w * scaleX);
                data.outH = static_cast<int>(inst.fbo_h * scaleY);
            } else {
                // This shouldn't happen in normal operation - mirror_thread always produces finalTexture
                data.texture = inst.fboTexture;
                data.tex_w = inst.fbo_w;
                data.tex_h = inst.fbo_h;
                data.outW = static_cast<int>(inst.fbo_w * scaleX);
                data.outH = static_cast<int>(inst.fbo_h * scaleY);
            }

            if (data.texture == 0) continue;

            // Copy the fence handle - we'll wait on it AFTER releasing the lock
            // The fence is not deleted here; it lives until SwapMirrorBuffers replaces it
            GLsync fence = inst.gpuFence;

            const auto& cache = inst.cachedRenderState;
            bool isAnimating = transitionProgress < 1.0f;
            bool cacheMatchesCurrentGeo =
                cache.isValid && !isAnimating && cache.finalX == geo.finalX && cache.finalY == geo.finalY && cache.finalW == geo.finalW &&
                cache.finalH == geo.finalH && cache.screenW == fullW && cache.screenH == fullH &&
                cache.outputX == conf.output.x && cache.outputY == conf.output.y && cache.outputScale == conf.output.scale &&
                cache.outputSeparateScale == conf.output.separateScale && cache.outputScaleX == conf.output.scaleX &&
                cache.outputScaleY == conf.output.scaleY && cache.outputRelativeTo == conf.output.relativeTo;

            if (cacheMatchesCurrentGeo) {
                memcpy(data.vertices, inst.cachedRenderState.vertices, sizeof(data.vertices));
                data.screenX = cache.mirrorScreenX;
                data.screenY = cache.mirrorScreenY;
                data.screenW = cache.mirrorScreenW;
                data.screenH = cache.mirrorScreenH;
                data.cacheValid = true;
            } else {
                data.cacheValid = false;
            }

            data.hasFrameContent = inst.hasFrameContent;

            data.gpuFence = nullptr;
            size_t idx = mirrorsToRender.size();
            mirrorsToRender.push_back(data);

            // Record fence for deferred wait (only if fence exists)
            if (fence) {
                pendingFences.push_back({ fence, idx });
            }
        }
    } // Lock released here - mirror thread is now unblocked

    // PHASE 2: Wait on GPU fences WITHOUT holding the mutex
    // This prevents priority inversion where the mirror thread can't acquire the lock
    // because we're holding it while blocking on a GPU fence.
    for (const auto& pf : pendingFences) {
        // Prefer GPU-side waits to avoid blocking the render thread CPU.
        if (pf.fence && glIsSync(pf.fence)) {
            glWaitSync(pf.fence, 0, GL_TIMEOUT_IGNORED);
        }
    }

    if (mirrorsToRender.empty()) return;

    glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);

    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glActiveTexture(GL_TEXTURE0);

    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    // All border rendering is now done by mirror_thread
    // Render thread just blits the pre-rendered finalTexture using passthrough shader
    glUseProgram(rt_backgroundProgram);

    for (auto& renderData : mirrorsToRender) {
        const MirrorConfig& conf = *renderData.config;
        const float effectiveOpacity = modeOpacity * conf.opacity;
        if (effectiveOpacity <= 0.0f) continue;
        glUniform1f(rt_backgroundShaderLocs.opacity, effectiveOpacity);

        glBindTexture(GL_TEXTURE_2D, renderData.texture);

        if (renderData.cacheValid) {
            glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(renderData.vertices), renderData.vertices);
        } else {
            std::string anchor = conf.output.relativeTo;

            bool isScreenRelative = false;
            if (anchor.length() > 6 && anchor.substr(anchor.length() - 6) == "Screen") {
                anchor = anchor.substr(0, anchor.length() - 6);
                isScreenRelative = true;
            } else if (anchor.length() > 8 && anchor.substr(anchor.length() - 8) == "Viewport") {
                anchor = anchor.substr(0, anchor.length() - 8);
            }

            int finalX_screen, finalY_screen, finalW_screen, finalH_screen;

            if (isScreenRelative) {
                int outX, outY;
                GetRelativeCoords(anchor, conf.output.x, conf.output.y, renderData.outW, renderData.outH, fullW, fullH, outX, outY);
                finalX_screen = outX;
                finalY_screen = outY;
                finalW_screen = renderData.outW;
                finalH_screen = renderData.outH;

                renderData.screenX = finalX_screen;
                renderData.screenY = finalY_screen;
                renderData.screenW = finalW_screen;
                renderData.screenH = finalH_screen;
            } else {
                // Must calculate position relative to EACH viewport's actual dimensions

                float toScaleX = (toW > 0 && geo.gameW > 0) ? static_cast<float>(toW) / geo.gameW : 1.0f;
                float toScaleY = (toH > 0 && geo.gameH > 0) ? static_cast<float>(toH) / geo.gameH : 1.0f;
                float fromScaleX = (fromW > 0 && geo.gameW > 0) ? static_cast<float>(fromW) / geo.gameW : toScaleX;
                float fromScaleY = (fromH > 0 && geo.gameH > 0) ? static_cast<float>(fromH) / geo.gameH : toScaleY;

                int toSizeW = relativeStretching ? static_cast<int>(renderData.outW * toScaleX) : renderData.outW;
                int toSizeH = relativeStretching ? static_cast<int>(renderData.outH * toScaleY) : renderData.outH;
                int fromSizeW = relativeStretching ? static_cast<int>(renderData.outW * fromScaleX) : renderData.outW;
                int fromSizeH = relativeStretching ? static_cast<int>(renderData.outH * fromScaleY) : renderData.outH;

                int toOutX, toOutY;
                GetRelativeCoords(anchor, conf.output.x, conf.output.y, toSizeW, toSizeH, toW, toH, toOutX, toOutY);
                int toPosX = toX + toOutX;
                int toPosY = toY + toOutY;

                int fromOutX, fromOutY;
                int effectiveFromH = isTransitioningFromEyeZoom ? toH : fromH;
                int effectiveFromY = isTransitioningFromEyeZoom ? toY : fromY;
                int effectiveFromSizeH = isTransitioningFromEyeZoom ? toSizeH : fromSizeH;
                GetRelativeCoords(anchor, conf.output.x, conf.output.y, fromSizeW, effectiveFromSizeH, fromW, effectiveFromH, fromOutX,
                                  fromOutY);
                int fromPosX = fromX + fromOutX;
                int fromPosY = effectiveFromY + fromOutY;

                float t = transitionProgress;
                finalX_screen = static_cast<int>(fromPosX + (toPosX - fromPosX) * t);
                finalY_screen = static_cast<int>(fromPosY + (toPosY - fromPosY) * t);

                if (relativeStretching) {
                    finalW_screen = static_cast<int>(fromSizeW + (toSizeW - fromSizeW) * t);
                    finalH_screen = static_cast<int>(fromSizeH + (toSizeH - fromSizeH) * t);
                } else {
                    finalW_screen = renderData.outW;
                    finalH_screen = renderData.outH;
                }

                renderData.screenX = finalX_screen;
                renderData.screenY = finalY_screen;
                renderData.screenW = finalW_screen;
                renderData.screenH = finalH_screen;
            }

            // 1. EyeZoom-specific: Uses eyeZoomAnimatedViewportX for precise viewport-synchronized slide

            bool shouldApplySlide = false;
            float slideProgress = 1.0f;

            // --- EyeZoom slide animation (uses viewport X for synchronization) ---
            auto ezCfgSnap = GetConfigSnapshot();
            if (!ezCfgSnap) continue;
            EyeZoomConfig zoomConfig = ezCfgSnap->eyezoom;
            int modeWidth = zoomConfig.windowWidth;
            int targetViewportX = (fullW - modeWidth) / 2;

            bool hasEyeZoomAnimatedPosition = eyeZoomAnimatedViewportX >= 0 && targetViewportX > 0;
            bool isEyeZoomTransitioning = hasEyeZoomAnimatedPosition && eyeZoomAnimatedViewportX < targetViewportX;

            bool isTransitioningToEyeZoom = isEyeZoomMode && isEyeZoomTransitioning && !isTransitioningFromEyeZoom;
            bool isEyeZoomSlideOut = isEyeZoomMode && isTransitioningFromEyeZoom && isEyeZoomTransitioning;

            if (zoomConfig.slideMirrorsIn && (isTransitioningToEyeZoom || isEyeZoomSlideOut) && hasEyeZoomAnimatedPosition) {
                shouldApplySlide = true;
                slideProgress = static_cast<float>(eyeZoomAnimatedViewportX) / static_cast<float>(targetViewportX);
            }

            if (!shouldApplySlide && mirrorSlideProgress < 1.0f && !skipAnimation) {
                if (toSlideMirrorsIn && !isSlideOutPass) {
                    shouldApplySlide = true;
                    slideProgress = mirrorSlideProgress;
                }
                else if (fromSlideMirrorsIn && isSlideOutPass) {
                    shouldApplySlide = true;
                    slideProgress = 1.0f - mirrorSlideProgress;
                }
            }

            if (shouldApplySlide && sourceMirrorNames.count(conf.name) > 0) { shouldApplySlide = false; }

            if (shouldApplySlide) {
                slideProgress = (slideProgress < 0.0f) ? 0.0f : (slideProgress > 1.0f ? 1.0f : slideProgress);

                int mirrorCenterX = finalX_screen + finalW_screen / 2;
                bool isOnLeftSide = mirrorCenterX < (fullW / 2);

                int offScreenLeft = -finalW_screen;
                int offScreenRight = fullW;

                if (isOnLeftSide) {
                    int slideX = offScreenLeft + static_cast<int>((finalX_screen - offScreenLeft) * slideProgress);
                    finalX_screen = slideX;
                } else {
                    int slideX = offScreenRight - static_cast<int>((offScreenRight - finalX_screen) * slideProgress);
                    finalX_screen = slideX;
                }

                renderData.screenX = finalX_screen;
            }

            int finalY_gl = fullH - finalY_screen - finalH_screen;

            float nx1 = (static_cast<float>(finalX_screen) / fullW) * 2.0f - 1.0f;
            float ny1 = (static_cast<float>(finalY_gl) / fullH) * 2.0f - 1.0f;
            float nx2 = (static_cast<float>(finalX_screen + finalW_screen) / fullW) * 2.0f - 1.0f;
            float ny2 = (static_cast<float>(finalY_gl + finalH_screen) / fullH) * 2.0f - 1.0f;

            float verts[] = { nx1, ny1, 0, 0, nx2, ny1, 1, 0, nx2, ny2, 1, 1, nx1, ny1, 0, 0, nx2, ny2, 1, 1, nx1, ny2, 0, 1 };
            glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
        }

        glDrawArrays(GL_TRIANGLES, 0, 6);
    }

    glUseProgram(rt_staticBorderProgram);

    for (const auto& renderData : mirrorsToRender) {
        const MirrorConfig& conf = *renderData.config;
        const MirrorBorderConfig& border = conf.border;

        if (border.type != MirrorBorderType::Static) continue;

        if (border.staticThickness <= 0) continue;

        if (!renderData.hasFrameContent) continue;

        if (renderData.screenW <= 0 || renderData.screenH <= 0) continue;

        int baseW = (border.staticWidth > 0) ? border.staticWidth : renderData.screenW;
        int baseH = (border.staticHeight > 0) ? border.staticHeight : renderData.screenH;
        baseW = (baseW < 2) ? 2 : baseW;
        baseH = (baseH < 2) ? 2 : baseH;

        int borderExtension = border.staticThickness + 1;
        int quadW = baseW + borderExtension * 2;
        int quadH = baseH + borderExtension * 2;

        int centerOffsetX = (baseW - renderData.screenW) / 2;
        int centerOffsetY = (baseH - renderData.screenH) / 2;

        int quadX = renderData.screenX - centerOffsetX + border.staticOffsetX - borderExtension;
        int quadY = renderData.screenY - centerOffsetY + border.staticOffsetY - borderExtension;

        glUniform1i(rt_staticBorderShaderLocs.shape, static_cast<int>(border.staticShape));
        glUniform4f(rt_staticBorderShaderLocs.borderColor, border.staticColor.r, border.staticColor.g, border.staticColor.b,
                    border.staticColor.a * conf.opacity * modeOpacity);
        glUniform1f(rt_staticBorderShaderLocs.thickness, static_cast<float>(border.staticThickness));
        glUniform1f(rt_staticBorderShaderLocs.radius, static_cast<float>(border.staticRadius));
        glUniform2f(rt_staticBorderShaderLocs.size, static_cast<float>(baseW), static_cast<float>(baseH));
        glUniform2f(rt_staticBorderShaderLocs.quadSize, static_cast<float>(quadW), static_cast<float>(quadH));

        int finalY_gl = fullH - (quadY + quadH);

        float nx1 = (static_cast<float>(quadX) / fullW) * 2.0f - 1.0f;
        float ny1 = (static_cast<float>(finalY_gl) / fullH) * 2.0f - 1.0f;
        float nx2 = (static_cast<float>(quadX + quadW) / fullW) * 2.0f - 1.0f;
        float ny2 = (static_cast<float>(finalY_gl + quadH) / fullH) * 2.0f - 1.0f;

        float verts[] = { nx1, ny1, 0, 0, nx2, ny1, 1, 0, nx2, ny2, 1, 1, nx1, ny1, 0, 0, nx2, ny2, 1, 1, nx1, ny2, 0, 1 };
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }

    glDisable(GL_BLEND);
}

// Render images using render thread's local shader programs
struct RT_UserImageCache {
    UserImageInstance::CachedImageRenderState cachedRenderState;
    bool filterInitialized = false;
    bool lastPixelatedScaling = false;
};

static std::unordered_map<std::string, RT_UserImageCache> g_rtUserImageCache;

static void RT_CalculateImageDimensionsFromTexture(int texWidth, int texHeight, const ImageConfig& img, int& outW, int& outH) {
    if (texWidth > 0 && texHeight > 0) {
        int croppedWidth = texWidth - img.crop_left - img.crop_right;
        int croppedHeight = texHeight - img.crop_top - img.crop_bottom;
        if (croppedWidth < 1) croppedWidth = 1;
        if (croppedHeight < 1) croppedHeight = 1;
        outW = static_cast<int>(croppedWidth * img.scale);
        outH = static_cast<int>(croppedHeight * img.scale);
        if (outW < 1) outW = 1;
        if (outH < 1) outH = 1;
    } else {
        outW = static_cast<int>(100 * img.scale);
        outH = static_cast<int>(100 * img.scale);
        if (outW < 1) outW = 1;
        if (outH < 1) outH = 1;
    }
}

static void RT_RenderImages(const std::vector<ImageConfig>& activeImages, int fullW, int fullH, int gameX, int gameY, int gameW, int gameH,
                            int gameResW, int gameResH, bool relativeStretching, float transitionProgress, int fromX, int fromY, int fromW,
                            int fromH, float modeOpacity, bool excludeOnlyOnMyScreen, GLuint vao, GLuint vbo) {
    if (activeImages.empty()) return;

    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glActiveTexture(GL_TEXTURE0);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    struct RT_ImageDrawInput {
        const ImageConfig* conf;
        GLuint texId;
        int texWidth;
        int texHeight;
        bool isFullyTransparent;
    };

    static thread_local std::vector<RT_ImageDrawInput> drawInputs;
    drawInputs.clear();
    drawInputs.reserve(activeImages.size());
    {
        std::lock_guard<std::mutex> lock(g_userImagesMutex);
        for (const auto& conf : activeImages) {
            if (excludeOnlyOnMyScreen && conf.onlyOnMyScreen) continue;
            auto it_inst = g_userImages.find(conf.name);
            if (it_inst == g_userImages.end() || it_inst->second.textureId == 0) continue;
            const UserImageInstance& inst = it_inst->second;
            drawInputs.push_back({ &conf, inst.textureId, inst.width, inst.height, inst.isFullyTransparent });
        }
    }

    for (const auto& in : drawInputs) {
        const ImageConfig& conf = *in.conf;
        const GLuint texId = in.texId;
        const int texWidth = in.texWidth;
        const int texHeight = in.texHeight;
        const bool isFullyTransparent = in.isFullyTransparent;

        RT_UserImageCache& rtInst = g_rtUserImageCache[conf.name];

        const float effectiveOpacity = conf.opacity * modeOpacity;
        const bool hasBg = conf.background.enabled && conf.background.opacity > 0.0f && !isFullyTransparent;
        const bool hasBorder = conf.border.enabled && conf.border.width > 0 && !isFullyTransparent;
        if (effectiveOpacity <= 0.0f && !hasBg && !hasBorder) { continue; }

        float nx1, ny1, nx2, ny2;
        int displayW, displayH;
        auto& cache = rtInst.cachedRenderState;

        // Check if config has changed - must match main thread's validation logic
        bool configChanged = !cache.isValid || cache.crop_left != conf.crop_left || cache.crop_right != conf.crop_right ||
                             cache.crop_top != conf.crop_top || cache.crop_bottom != conf.crop_bottom || cache.scale != conf.scale ||
                             cache.x != conf.x || cache.y != conf.y || cache.relativeTo != conf.relativeTo || cache.screenWidth != fullW ||
                             cache.screenHeight != fullH;

        if (!configChanged) {
            nx1 = cache.nx1;
            ny1 = cache.ny1;
            nx2 = cache.nx2;
            ny2 = cache.ny2;
            displayW = cache.displayW;
            displayH = cache.displayH;
        } else {
            RT_CalculateImageDimensionsFromTexture(texWidth, texHeight, conf, displayW, displayH);

            bool isViewportRelative = conf.relativeTo.length() > 8 && conf.relativeTo.substr(conf.relativeTo.length() - 8) == "Viewport";

            int finalScreenX_win, finalScreenY_win;
            int finalDisplayW = displayW;
            int finalDisplayH = displayH;

            if (isViewportRelative) {
                float toScaleX = (gameW > 0 && gameResW > 0) ? static_cast<float>(gameW) / gameResW : 1.0f;
                float toScaleY = (gameH > 0 && gameResH > 0) ? static_cast<float>(gameH) / gameResH : 1.0f;
                float fromScaleX = (fromW > 0 && gameResW > 0) ? static_cast<float>(fromW) / gameResW : toScaleX;
                float fromScaleY = (fromH > 0 && gameResH > 0) ? static_cast<float>(fromH) / gameResH : toScaleY;

                int toDisplayW = relativeStretching ? static_cast<int>(displayW * toScaleX) : displayW;
                int toDisplayH = relativeStretching ? static_cast<int>(displayH * toScaleY) : displayH;
                int fromDisplayW = relativeStretching ? static_cast<int>(displayW * fromScaleX) : displayW;
                int fromDisplayH = relativeStretching ? static_cast<int>(displayH * fromScaleY) : displayH;

                int toPosX, toPosY;
                GetRelativeCoordsForImageWithViewport(conf.relativeTo, conf.x, conf.y, toDisplayW, toDisplayH, gameX, gameY, gameW, gameH,
                                                      fullW, fullH, toPosX, toPosY);

                int fromPosX, fromPosY;
                GetRelativeCoordsForImageWithViewport(conf.relativeTo, conf.x, conf.y, fromDisplayW, fromDisplayH, fromX, fromY, fromW,
                                                      fromH, fullW, fullH, fromPosX, fromPosY);

                float t = transitionProgress;
                finalScreenX_win = static_cast<int>(fromPosX + (toPosX - fromPosX) * t);
                finalScreenY_win = static_cast<int>(fromPosY + (toPosY - fromPosY) * t);

                if (relativeStretching) {
                    finalDisplayW = static_cast<int>(fromDisplayW + (toDisplayW - fromDisplayW) * t);
                    finalDisplayH = static_cast<int>(fromDisplayH + (toDisplayH - fromDisplayH) * t);
                }
            } else {
                GetRelativeCoordsForImageWithViewport(conf.relativeTo, conf.x, conf.y, finalDisplayW, finalDisplayH, gameX, gameY, gameW,
                                                      gameH, fullW, fullH, finalScreenX_win, finalScreenY_win);
            }

            int finalScreenY_gl = fullH - finalScreenY_win - finalDisplayH;
            nx1 = (static_cast<float>(finalScreenX_win) / fullW) * 2.0f - 1.0f;
            ny1 = (static_cast<float>(finalScreenY_gl) / fullH) * 2.0f - 1.0f;
            nx2 = (static_cast<float>(finalScreenX_win + finalDisplayW) / fullW) * 2.0f - 1.0f;
            ny2 = (static_cast<float>(finalScreenY_gl + finalDisplayH) / fullH) * 2.0f - 1.0f;
            displayW = finalDisplayW;
            displayH = finalDisplayH;

            // Update render-thread-local cache
            cache.crop_left = conf.crop_left;
            cache.crop_right = conf.crop_right;
            cache.crop_top = conf.crop_top;
            cache.crop_bottom = conf.crop_bottom;
            cache.scale = conf.scale;
            cache.x = conf.x;
            cache.y = conf.y;
            cache.relativeTo = conf.relativeTo;
            cache.screenWidth = fullW;
            cache.screenHeight = fullH;
            cache.displayW = displayW;
            cache.displayH = displayH;
            cache.nx1 = nx1;
            cache.ny1 = ny1;
            cache.nx2 = nx2;
            cache.ny2 = ny2;
            cache.isValid = true;
        }

        if (hasBg) {
            glUseProgram(rt_solidColorProgram);
            glUniform4f(rt_solidColorShaderLocs.color, conf.background.color.r, conf.background.color.g, conf.background.color.b,
                        conf.background.opacity * modeOpacity);
            float bg_verts[] = { nx1, ny1, 0, 0, nx2, ny1, 0, 0, nx2, ny2, 0, 0, nx1, ny1, 0, 0, nx2, ny2, 0, 0, nx1, ny2, 0, 0 };
            glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(bg_verts), bg_verts);
            glDrawArrays(GL_TRIANGLES, 0, 6);
        }

        glUseProgram(rt_imageRenderProgram);
        glBindTexture(GL_TEXTURE_2D, texId);

        if (!rtInst.filterInitialized || rtInst.lastPixelatedScaling != conf.pixelatedScaling) {
            if (conf.pixelatedScaling) {
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            } else {
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            }
            rtInst.lastPixelatedScaling = conf.pixelatedScaling;
            rtInst.filterInitialized = true;
        }
        glUniform1i(rt_imageRenderShaderLocs.enableColorKey, conf.enableColorKey && !conf.colorKeys.empty() ? 1 : 0);
        if (conf.enableColorKey && !conf.colorKeys.empty()) {
            glUniform3f(rt_imageRenderShaderLocs.colorKey, conf.colorKeys[0].color.r, conf.colorKeys[0].color.g, conf.colorKeys[0].color.b);
            glUniform1f(rt_imageRenderShaderLocs.sensitivity, conf.colorKeys[0].sensitivity);
        }
        glUniform1f(rt_imageRenderShaderLocs.opacity, effectiveOpacity);

        const float invW = (texWidth > 0) ? (1.0f / texWidth) : 0.0f;
        const float invH = (texHeight > 0) ? (1.0f / texHeight) : 0.0f;
        float tu1 = conf.crop_left * invW;
        float tu2 = (texWidth - conf.crop_right) * invW;
        float tv1 = conf.crop_bottom * invH;
        float tv2 = (texHeight - conf.crop_top) * invH;

        float verts[] = { nx1, ny1, tu1, tv1, nx2, ny1, tu2, tv1, nx2, ny2, tu2, tv2,
                          nx1, ny1, tu1, tv1, nx2, ny2, tu2, tv2, nx1, ny2, tu1, tv2 };
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        if (hasBorder) {
            int finalScreenX_win = static_cast<int>((nx1 + 1.0f) / 2.0f * fullW);
            int finalScreenY_gl = static_cast<int>((ny1 + 1.0f) / 2.0f * fullH);
            int finalScreenY_win = fullH - finalScreenY_gl - displayH;

            RT_RenderGameBorder(finalScreenX_win, finalScreenY_win, displayW, displayH, conf.border.width, conf.border.radius,
                                conf.border.color, fullW, fullH, vao, vbo);

            glBindVertexArray(vao);
            glBindBuffer(GL_ARRAY_BUFFER, vbo);
        }
    }

    glDisable(GL_BLEND);
}

// Render window overlays using render thread's local shader programs
static void RT_RenderWindowOverlays(const std::vector<const WindowOverlayConfig*>& overlays, int fullW, int fullH, int gameX, int gameY,
                                    int gameW, int gameH, int gameResW, int gameResH, bool relativeStretching, float transitionProgress,
                                    int fromX, int fromY, int fromW, int fromH, float modeOpacity, bool excludeOnlyOnMyScreen, GLuint vao,
                                    GLuint vbo) {
    if (overlays.empty()) return;

    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glActiveTexture(GL_TEXTURE0);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(rt_imageRenderProgram);
    glUniform1i(rt_imageRenderShaderLocs.enableColorKey, 0);
    glUniform1f(rt_imageRenderShaderLocs.opacity, modeOpacity);

    std::unique_lock<std::mutex> cacheLock(g_windowOverlayCacheMutex, std::try_to_lock);
    if (!cacheLock.owns_lock()) {
        glDisable(GL_BLEND);
        return; // Skip if can't get lock
    }

    const std::string focusedName = GetFocusedWindowOverlayName();

    for (const WindowOverlayConfig* conf : overlays) {
        if (!conf) continue;
        if (excludeOnlyOnMyScreen && conf->onlyOnMyScreen) continue;

        const std::string& overlayId = conf->name;

        const float effectiveOpacity = conf->opacity * modeOpacity;
        const bool hasBg = conf->background.enabled && conf->background.opacity > 0.0f;
        const bool hasBorder = conf->border.enabled && conf->border.width > 0;
        if (effectiveOpacity <= 0.0f && !hasBg && !hasBorder) { continue; }

        auto it = g_windowOverlayCache.find(overlayId);
        if (it == g_windowOverlayCache.end() || !it->second) continue;

        WindowOverlayCacheEntry& entry = *it->second;

        // Check if capture thread has a new frame ready
        if (entry.hasNewFrame.load(std::memory_order_acquire)) {
            // Swap readyBuffer with backBuffer under lock - this gives us exclusive access to backBuffer
            {
                std::lock_guard<std::mutex> lock(entry.swapMutex);
                entry.readyBuffer.swap(entry.backBuffer);
            }
            entry.hasNewFrame.store(false, std::memory_order_release);
        }

        // Now read from backBuffer - it's safe, capture thread won't touch it
        WindowOverlayRenderData* renderData = entry.backBuffer.get();
        if (renderData && renderData->pixelData && renderData->width > 0 && renderData->height > 0) {
            if (renderData != entry.lastUploadedRenderData) {
                if (entry.glTextureId == 0) {
                    glGenTextures(1, &entry.glTextureId);
                    glBindTexture(GL_TEXTURE_2D, entry.glTextureId);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                    entry.filterInitialized = false;
                }

                glBindTexture(GL_TEXTURE_2D, entry.glTextureId);

                if (entry.glTextureWidth != renderData->width || entry.glTextureHeight != renderData->height) {
                    entry.glTextureWidth = renderData->width;
                    entry.glTextureHeight = renderData->height;
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, renderData->width, renderData->height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                                 renderData->pixelData);
                } else {
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, renderData->width, renderData->height, GL_RGBA, GL_UNSIGNED_BYTE,
                                    renderData->pixelData);
                }

                entry.lastUploadedRenderData = renderData;
            }
        }

        if (entry.glTextureId == 0) continue;

        int croppedW = entry.glTextureWidth - conf->crop_left - conf->crop_right;
        int croppedH = entry.glTextureHeight - conf->crop_top - conf->crop_bottom;
        if (croppedW < 1) croppedW = 1;
        if (croppedH < 1) croppedH = 1;
        int displayW = static_cast<int>(croppedW * conf->scale);
        int displayH = static_cast<int>(croppedH * conf->scale);
        if (displayW < 1) displayW = 1;
        if (displayH < 1) displayH = 1;

        bool isViewportRelative = conf->relativeTo.length() > 8 && conf->relativeTo.substr(conf->relativeTo.length() - 8) == "Viewport";

        int screenX, screenY;

        if (isViewportRelative) {
            float toScaleX = (gameW > 0 && gameResW > 0) ? static_cast<float>(gameW) / gameResW : 1.0f;
            float toScaleY = (gameH > 0 && gameResH > 0) ? static_cast<float>(gameH) / gameResH : 1.0f;
            float fromScaleX = (fromW > 0 && gameResW > 0) ? static_cast<float>(fromW) / gameResW : toScaleX;
            float fromScaleY = (fromH > 0 && gameResH > 0) ? static_cast<float>(fromH) / gameResH : toScaleY;

            int toDisplayW = relativeStretching ? static_cast<int>(displayW * toScaleX) : displayW;
            int toDisplayH = relativeStretching ? static_cast<int>(displayH * toScaleY) : displayH;
            int fromDisplayW = relativeStretching ? static_cast<int>(displayW * fromScaleX) : displayW;
            int fromDisplayH = relativeStretching ? static_cast<int>(displayH * fromScaleY) : displayH;

            int toPosX, toPosY;
            GetRelativeCoordsForImageWithViewport(conf->relativeTo, conf->x, conf->y, toDisplayW, toDisplayH, gameX, gameY, gameW, gameH,
                                                  fullW, fullH, toPosX, toPosY);

            int fromPosX, fromPosY;
            GetRelativeCoordsForImageWithViewport(conf->relativeTo, conf->x, conf->y, fromDisplayW, fromDisplayH, fromX, fromY, fromW,
                                                  fromH, fullW, fullH, fromPosX, fromPosY);

            float t = transitionProgress;
            screenX = static_cast<int>(fromPosX + (toPosX - fromPosX) * t);
            screenY = static_cast<int>(fromPosY + (toPosY - fromPosY) * t);

            if (relativeStretching) {
                displayW = static_cast<int>(fromDisplayW + (toDisplayW - fromDisplayW) * t);
                displayH = static_cast<int>(fromDisplayH + (toDisplayH - fromDisplayH) * t);
            }
        } else {
            GetRelativeCoordsForImageWithViewport(conf->relativeTo, conf->x, conf->y, displayW, displayH, gameX, gameY, gameW, gameH, fullW,
                                                  fullH, screenX, screenY);
        }

        int screenY_gl = fullH - screenY - displayH;

        float nx1 = (static_cast<float>(screenX) / fullW) * 2.0f - 1.0f;
        float ny1 = (static_cast<float>(screenY_gl) / fullH) * 2.0f - 1.0f;
        float nx2 = (static_cast<float>(screenX + displayW) / fullW) * 2.0f - 1.0f;
        float ny2 = (static_cast<float>(screenY_gl + displayH) / fullH) * 2.0f - 1.0f;

        if (hasBg) {
            glUseProgram(rt_solidColorProgram);
            glUniform4f(rt_solidColorShaderLocs.color, conf->background.color.r, conf->background.color.g, conf->background.color.b,
                        conf->background.opacity * modeOpacity);
            float bg_verts[] = { nx1, ny1, 0, 0, nx2, ny1, 0, 0, nx2, ny2, 0, 0, nx1, ny1, 0, 0, nx2, ny2, 0, 0, nx1, ny2, 0, 0 };
            glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(bg_verts), bg_verts);
            glDrawArrays(GL_TRIANGLES, 0, 6);
        }

        glUseProgram(rt_imageRenderProgram);
        glBindTexture(GL_TEXTURE_2D, entry.glTextureId);

        if (!entry.filterInitialized || entry.lastPixelatedScaling != conf->pixelatedScaling) {
            if (conf->pixelatedScaling) {
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            } else {
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            }
            entry.lastPixelatedScaling = conf->pixelatedScaling;
            entry.filterInitialized = true;
        }

        glUniform1i(rt_imageRenderShaderLocs.enableColorKey, 0);
        glUniform1f(rt_imageRenderShaderLocs.opacity, effectiveOpacity);

        float tu1 = static_cast<float>(conf->crop_left) / entry.glTextureWidth;
        float tv1 = static_cast<float>(conf->crop_top) / entry.glTextureHeight;
        float tu2 = static_cast<float>(entry.glTextureWidth - conf->crop_right) / entry.glTextureWidth;
        float tv2 = static_cast<float>(entry.glTextureHeight - conf->crop_bottom) / entry.glTextureHeight;

        float verts[] = { nx1, ny1, tu1, tv2, nx2, ny1, tu2, tv2, nx2, ny2, tu2, tv1,
                          nx1, ny1, tu1, tv2, nx2, ny2, tu2, tv1, nx1, ny2, tu1, tv1 };
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        if (hasBorder) {
            RT_RenderGameBorder(screenX, screenY, displayW, displayH, conf->border.width, conf->border.radius, conf->border.color, fullW,
                                fullH, vao, vbo);

            glBindVertexArray(vao);
            glBindBuffer(GL_ARRAY_BUFFER, vbo);
        }

        if (!focusedName.empty() && focusedName == overlayId) {
            Color focusedBorderColor = { 0.0f, 1.0f, 0.0f, 1.0f };
            int focusedBorderWidth = 3;
            int focusedBorderRadius = conf->border.enabled ? conf->border.radius : 0;

            RT_RenderGameBorder(screenX, screenY, displayW, displayH, focusedBorderWidth, focusedBorderRadius, focusedBorderColor, fullW,
                                fullH, vao, vbo);

            glBindVertexArray(vao);
            glBindBuffer(GL_ARRAY_BUFFER, vbo);
        }
    }

    glDisable(GL_BLEND);
}

// This runs on the render thread, moving the work off the main thread
static void RT_CollectActiveElements(const Config& config, const std::string& modeId, bool onlyOnMyScreenPass,
                                     std::vector<MirrorConfig>& outMirrors, std::vector<ImageConfig>& outImages,
                                     std::vector<const WindowOverlayConfig*>& outWindowOverlays) {
    outMirrors.clear();
    outImages.clear();
    outWindowOverlays.clear();

    // These caches are safe because `config` is immutable for the lifetime of the snapshot.
    static const Config* s_cachedConfigPtr = nullptr;
    static std::unordered_map<std::string, const ModeConfig*> s_modeById;
    static std::unordered_map<std::string, const MirrorConfig*> s_mirrorByName;
    static std::unordered_map<std::string, const MirrorGroupConfig*> s_groupByName;
    static std::unordered_map<std::string, const ImageConfig*> s_imageByName;
    static std::unordered_map<std::string, const WindowOverlayConfig*> s_windowOverlayByName;

    if (s_cachedConfigPtr != &config) {
        s_cachedConfigPtr = &config;
        s_modeById.clear();
        s_mirrorByName.clear();
        s_groupByName.clear();
        s_imageByName.clear();
        s_windowOverlayByName.clear();

        s_modeById.reserve(config.modes.size());
        for (const auto& m : config.modes) { s_modeById[m.id] = &m; }

        s_mirrorByName.reserve(config.mirrors.size());
        for (const auto& m : config.mirrors) { s_mirrorByName[m.name] = &m; }

        s_groupByName.reserve(config.mirrorGroups.size());
        for (const auto& g : config.mirrorGroups) { s_groupByName[g.name] = &g; }

        s_imageByName.reserve(config.images.size());
        for (const auto& img : config.images) { s_imageByName[img.name] = &img; }

        s_windowOverlayByName.reserve(config.windowOverlays.size());
        for (const auto& o : config.windowOverlays) { s_windowOverlayByName[o.name] = &o; }
    }

    const ModeConfig* mode = nullptr;
    if (auto it = s_modeById.find(modeId); it != s_modeById.end()) {
        mode = it->second;
    } else {
        for (const auto& kv : s_modeById) {
            if (EqualsIgnoreCase(kv.first, modeId)) {
                mode = kv.second;
                break;
            }
        }
    }
    if (!mode) return;

    outMirrors.reserve(mode->mirrorIds.size() + mode->mirrorGroupIds.size());
    outImages.reserve(mode->imageIds.size());
    outWindowOverlays.reserve(mode->windowOverlayIds.size());

    for (const auto& mirrorName : mode->mirrorIds) {
        auto it = s_mirrorByName.find(mirrorName);
        if (it == s_mirrorByName.end() || !it->second) continue;
        const MirrorConfig& mirror = *it->second;
        if (!onlyOnMyScreenPass || mirror.onlyOnMyScreen) { outMirrors.push_back(mirror); }
    }

    for (const auto& groupName : mode->mirrorGroupIds) {
        auto git = s_groupByName.find(groupName);
        if (git == s_groupByName.end() || !git->second) continue;
        const auto& group = *git->second;

            for (const auto& item : group.mirrors) {
                if (!item.enabled) continue;
                auto mit = s_mirrorByName.find(item.mirrorId);
                if (mit != s_mirrorByName.end() && mit->second) {
                    const auto& mirror = *mit->second;
                    if (!onlyOnMyScreenPass || mirror.onlyOnMyScreen) {
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
                        outMirrors.push_back(groupedMirror);
                    }
                }
            }
    }

    if (g_imageOverlaysVisible.load(std::memory_order_acquire)) {
        for (const auto& imageName : mode->imageIds) {
            auto it = s_imageByName.find(imageName);
            if (it == s_imageByName.end() || !it->second) continue;
            const ImageConfig& image = *it->second;
            if (!onlyOnMyScreenPass || image.onlyOnMyScreen) { outImages.push_back(image); }
        }
    }

    if (g_windowOverlaysVisible.load(std::memory_order_acquire)) {
        for (const auto& overlayId : mode->windowOverlayIds) {
            auto it = s_windowOverlayByName.find(overlayId);
            if (it == s_windowOverlayByName.end() || !it->second) continue;
            const WindowOverlayConfig& overlay = *it->second;
            if (!onlyOnMyScreenPass || overlay.onlyOnMyScreen) { outWindowOverlays.push_back(it->second); }
        }
    }
}

static void RenderThreadFunc(void* gameGLContext) {
    _set_se_translator(SEHTranslator);

    try {
        Log("Render Thread: Starting...");

        if (!g_renderThreadDC || !g_renderThreadContext) {
            Log("Render Thread: Missing pre-created context or DC");
            g_renderThreadRunning.store(false);
            return;
        }

        // Make context current on this thread
        if (!wglMakeCurrent(g_renderThreadDC, g_renderThreadContext)) {
            Log("Render Thread: Failed to make context current (error " + std::to_string(GetLastError()) + ")");
            g_renderThreadRunning.store(false);
            return;
        }

        if (glewInit() != GLEW_OK) {
            Log("Render Thread: GLEW init failed");
            wglMakeCurrent(NULL, NULL);
            g_renderThreadRunning.store(false);
            return;
        }

        LogCategory("init", "Render Thread: Context initialized successfully");

        if (!RT_InitializeShaders()) {
            Log("Render Thread: Shader initialization failed");
            wglMakeCurrent(NULL, NULL);
            g_renderThreadRunning.store(false);
            return;
        }

        auto initCfg = GetConfigSnapshot();
        if (initCfg && initCfg->debug.virtualCameraEnabled) {
            int screenW = GetCachedWindowWidth();
            int screenH = GetCachedWindowHeight();
            int vcW, vcH;
            GetVirtualCamScaledSize(screenW, screenH, 1.0f, vcW, vcH);
            if (StartVirtualCamera(vcW, vcH, initCfg->debug.virtualCameraFps)) {
                LogCategory("init", "Render Thread: Virtual Camera initialized at " + std::to_string(vcW) + "x" + std::to_string(vcH) +
                                        " @ " + std::to_string(initCfg->debug.virtualCameraFps) + "fps");
            } else {
                Log("Render Thread: Virtual Camera initialization failed");
            }
        }

        GLuint renderVAO = 0, renderVBO = 0;
        glGenVertexArrays(1, &renderVAO);
        glGenBuffers(1, &renderVBO);
        glBindVertexArray(renderVAO);
        glBindBuffer(GL_ARRAY_BUFFER, renderVBO);
        glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 24, nullptr, GL_DYNAMIC_DRAW);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
        glEnableVertexAttribArray(1);

        int lastWidth = 0, lastHeight = 0;

        // Initialize ImGui on render thread
        {
            HWND hwnd = g_minecraftHwnd.load();
            if (hwnd) {
                IMGUI_CHECKVERSION();
                g_renderThreadImGuiContext = ImGui::CreateContext();
                ImGui::SetCurrentContext(g_renderThreadImGuiContext);

                ImGuiIO& io = ImGui::GetIO();
                io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

                const int screenHeight = GetCachedWindowHeight();
                float scaleFactor = 1.0f;
                if (screenHeight > 1080) { scaleFactor = static_cast<float>(screenHeight) / 1080.0f; }
                scaleFactor = roundf(scaleFactor * 4.0f) / 4.0f;
                if (scaleFactor < 1.0f) { scaleFactor = 1.0f; }
                g_eyeZoomScaleFactor = scaleFactor;

                auto fontCfg = GetConfigSnapshot();
                if (!fontCfg) {
                    Log("Render Thread: Config snapshot not available for font loading, using defaults");
                    fontCfg = std::make_shared<const Config>();
                }
                const Config& fontCfgRef = *fontCfg;
                std::string fontPath = fontCfgRef.fontPath;
                (void)RT_AddFontWithArialFallback(io.Fonts, fontPath, 16.0f * scaleFactor, "base font");

                std::string eyeZoomFontPath =
                    fontCfgRef.eyezoom.textFontPath.empty() ? fontCfgRef.fontPath : fontCfgRef.eyezoom.textFontPath;
                g_eyeZoomTextFont = RT_AddFontWithArialFallback(io.Fonts, eyeZoomFontPath, 80.0f * scaleFactor, "EyeZoom font", &g_eyeZoomFontPathCached);
                if (g_eyeZoomFontPathCached.empty()) { g_eyeZoomFontPathCached = ConfigDefaults::CONFIG_FONT_PATH; }

                ImGui::StyleColorsDark();
                LoadTheme();
                ApplyAppearanceConfig();
                ImGui::GetStyle().ScaleAllSizes(scaleFactor);

                ImGui_ImplWin32_Init(hwnd);
                ImGui_ImplOpenGL3_Init("#version 330");

                InitializeOverlayTextFont(fontPath, 16.0f, scaleFactor);

                g_fontsValid = true;
                g_renderThreadImGuiInitialized = true;
                LogCategory("init", "Render Thread: ImGui initialized successfully");
            } else {
                LogCategory("init", "Render Thread: HWND not available, ImGui not initialized");
            }
        }

        LogCategory("init", "Render Thread: Entering main loop");

        while (!g_renderThreadShouldStop.load()) {
            // Wait for frame request (lock only held during wait, not during processing)
            FrameRenderRequest request;
            bool isObsRequest = false;

            {
                std::unique_lock<std::mutex> lock(g_requestSignalMutex);
                g_requestCV.wait(lock, [] {
                    return g_requestReadySlot.load(std::memory_order_acquire) != -1 || g_obsReadySlot.load(std::memory_order_acquire) != -1 ||
                           g_renderThreadShouldStop.load();
                });
            }
            // Lock released - we don't hold it while processing

            if (g_renderThreadShouldStop.load()) break;

            int obsSlot = g_obsReadySlot.exchange(-1, std::memory_order_acq_rel);
            int mainSlot = g_requestReadySlot.exchange(-1, std::memory_order_acq_rel);
            bool hasObsRequest = (obsSlot != -1);
            bool hasMainRequest = (mainSlot != -1);

            if (!hasObsRequest && !hasMainRequest) {
                continue;
            }

            if (hasObsRequest) {
                PROFILE_SCOPE_CAT("RT Build OBS Request", "Render Thread");
                g_obsReadSlot.store(obsSlot, std::memory_order_release);
                ObsFrameSubmission submission = g_obsSubmissionSlots[obsSlot];
                g_obsReadSlot.store(-1, std::memory_order_release);
                // Build the full request on the render thread (deferred from main thread)
                request = BuildObsFrameRequest(submission.context, submission.isDualRenderingPath);
                request.gameTextureFence = submission.gameTextureFence;
                isObsRequest = true;
            } else {
                g_requestReadSlot.store(mainSlot, std::memory_order_release);
                request = g_requestSlots[mainSlot];
                g_requestReadSlot.store(-1, std::memory_order_release);
                isObsRequest = false;
            }

            FrameRenderRequest pendingMainRequest;
            bool hasPendingMain = hasObsRequest && hasMainRequest;
            if (hasPendingMain) {
                g_requestReadSlot.store(mainSlot, std::memory_order_release);
                pendingMainRequest = g_requestSlots[mainSlot];
                g_requestReadSlot.store(-1, std::memory_order_release);
            }

        process_request:

            auto startTime = std::chrono::high_resolution_clock::now();

            auto cfgSnapshot = GetConfigSnapshot();
            if (!cfgSnapshot) continue;
            const Config& cfg = *cfgSnapshot;

            // === Image Processing (moved from main thread) ===
            {
                PROFILE_SCOPE_CAT("RT Image Processing", "Render Thread");
                std::vector<DecodedImageData> imagesToProcess;
                {
                    std::lock_guard<std::mutex> lock(g_decodedImagesMutex);
                    if (!g_decodedImagesQueue.empty()) { imagesToProcess.swap(g_decodedImagesQueue); }
                }
                if (!imagesToProcess.empty()) {
                    for (const auto& decodedImg : imagesToProcess) {
                        UploadDecodedImageToGPU(decodedImg);
                        if (decodedImg.data) { stbi_image_free(decodedImg.data); }
                    }
                }
            }

            if (request.fullW != lastWidth || request.fullH != lastHeight) {
                InitRenderFBOs(request.fullW, request.fullH);
                lastWidth = request.fullW;
                lastHeight = request.fullH;
            }

            RenderFBO* fboArray;
            std::atomic<int>* writeFBOIndexPtr;
            if (isObsRequest) {
                fboArray = g_obsRenderFBOs;
                writeFBOIndexPtr = &g_obsWriteFBOIndex;
            } else {
                fboArray = g_renderFBOs;
                writeFBOIndexPtr = &g_writeFBOIndex;
            }

            int writeIdx = writeFBOIndexPtr->load();
            RenderFBO& writeFBO = fboArray[writeIdx];

            // Ensure the main thread has finished sampling this FBO's texture before we overwrite it.
            RT_WaitForConsumerFence(isObsRequest, writeIdx);

            glBindFramebuffer(GL_FRAMEBUFFER, writeFBO.fbo);
            if (oglViewport)
                oglViewport(0, 0, request.fullW, request.fullH);
            else
                glViewport(0, 0, request.fullW, request.fullH);

            if (isObsRequest) {
                glDisable(GL_SCISSOR_TEST);
                glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                glClearColor(request.bgR, request.bgG, request.bgB, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT);

                if (!request.isRawWindowedMode) {
                    std::string bgModeId = request.modeId;
                    if (request.isTransitioningFromEyeZoom) {
                        bgModeId = "EyeZoom";
                    }
                    else if (EqualsIgnoreCase(request.modeId, "Fullscreen") && !request.fromModeId.empty()) {
                        bgModeId = request.fromModeId;
                    }

                    const ModeConfig* mode = nullptr;
                    for (const auto& m : cfg.modes) {
                        if (EqualsIgnoreCase(m.id, bgModeId)) {
                            mode = &m;
                            break;
                        }
                    }

                    if (mode && mode->background.selectedMode == "gradient" && mode->background.gradientStops.size() >= 2) {
                        glUseProgram(rt_gradientProgram);
                        glBindVertexArray(renderVAO);
                        glBindBuffer(GL_ARRAY_BUFFER, renderVBO);

                        int numStops = (std::min)(static_cast<int>(mode->background.gradientStops.size()), 8);
                        glUniform1i(rt_gradientShaderLocs.numStops, numStops);

                        float colors[8 * 4];
                        float positions[8];
                        for (int i = 0; i < numStops; i++) {
                            colors[i * 4 + 0] = mode->background.gradientStops[i].color.r;
                            colors[i * 4 + 1] = mode->background.gradientStops[i].color.g;
                            colors[i * 4 + 2] = mode->background.gradientStops[i].color.b;
                            colors[i * 4 + 3] = 1.0f;
                            positions[i] = mode->background.gradientStops[i].position;
                        }
                        glUniform4fv(rt_gradientShaderLocs.stopColors, numStops, colors);
                        glUniform1fv(rt_gradientShaderLocs.stopPositions, numStops, positions);
                        glUniform1f(rt_gradientShaderLocs.angle, mode->background.gradientAngle * 3.14159265f / 180.0f);

                        static auto startTime = std::chrono::steady_clock::now();
                        auto now = std::chrono::steady_clock::now();
                        float timeSeconds = std::chrono::duration<float>(now - startTime).count();
                        glUniform1f(rt_gradientShaderLocs.time, timeSeconds);
                        glUniform1i(rt_gradientShaderLocs.animationType, static_cast<int>(mode->background.gradientAnimation));
                        glUniform1f(rt_gradientShaderLocs.animationSpeed, mode->background.gradientAnimationSpeed);
                        glUniform1i(rt_gradientShaderLocs.colorFade, mode->background.gradientColorFade ? 1 : 0);

                        float bgVerts[] = { -1.0f, -1.0f, 0.0f, 0.0f, 1.0f, -1.0f, 1.0f, 0.0f, 1.0f,  1.0f, 1.0f, 1.0f,
                                            -1.0f, -1.0f, 0.0f, 0.0f, 1.0f, 1.0f,  1.0f, 1.0f, -1.0f, 1.0f, 0.0f, 1.0f };
                        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(bgVerts), bgVerts);
                        glDrawArrays(GL_TRIANGLES, 0, 6);
                    } else if (mode && mode->background.selectedMode == "image") {
                        GLuint bgTex = 0;
                        {
                            std::lock_guard<std::mutex> bgLock(g_backgroundTexturesMutex);
                            auto bgTexIt = g_backgroundTextures.find(bgModeId);
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

                        if (bgTex != 0) {
                            glUseProgram(rt_backgroundProgram);
                            glBindVertexArray(renderVAO);
                            glBindBuffer(GL_ARRAY_BUFFER, renderVBO);
                            glActiveTexture(GL_TEXTURE0);
                            glBindTexture(GL_TEXTURE_2D, bgTex);
                            glUniform1i(rt_backgroundShaderLocs.backgroundTexture, 0);
                            glUniform1f(rt_backgroundShaderLocs.opacity, 1.0f);

                            float bgVerts[] = { -1.0f, -1.0f, 0.0f, 0.0f, 1.0f, -1.0f, 1.0f, 0.0f, 1.0f,  1.0f, 1.0f, 1.0f,
                                                -1.0f, -1.0f, 0.0f, 0.0f, 1.0f, 1.0f,  1.0f, 1.0f, -1.0f, 1.0f, 0.0f, 1.0f };
                            glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(bgVerts), bgVerts);
                            glDrawArrays(GL_TRIANGLES, 0, 6);
                        }
                    }
                }

                // Use the READY frame texture - guaranteed complete by mirror thread
                // No fence wait needed - mirror thread already waited on the fence
                // immediately after fence signals in Phase 1 of mirror thread loop
                GLuint readyTex = GetReadyGameTexture();
                int srcW = GetReadyGameWidth();
                int srcH = GetReadyGameHeight();

                // GetSafeReadTexture returns the texture NOT being written to (always valid, no fence needed)
                if (readyTex == 0 || srcW <= 0 || srcH <= 0) {
                    GLuint safeTex = GetSafeReadTexture();
                    if (safeTex != 0) {
                        readyTex = safeTex;
                        srcW = GetFallbackGameWidth();
                        srcH = GetFallbackGameHeight();
                        if (srcW <= 0 || srcH <= 0) {
                            srcW = request.fullW;
                            srcH = request.fullH;
                        }
                    }
                }

                if (readyTex != 0 && srcW > 0 && srcH > 0) {
                    int uvSrcW = srcW;
                    int uvSrcH = srcH;
                    if (request.isPre113Windowed && request.windowW > 0 && request.windowH > 0) {
                        uvSrcW = request.windowW;
                        uvSrcH = request.windowH;
                    }

                    // No fence wait needed for ready frame - frame is guaranteed complete
                    RT_RenderGameTexture(readyTex, request.animatedX, request.animatedY, request.animatedW, request.animatedH,
                                         request.fullW, request.fullH, uvSrcW, uvSrcH, srcW,
                                         srcH,
                                         renderVAO, renderVBO);

                    if (!request.isRawWindowedMode && request.transitioningToFullscreen && request.fromBorderEnabled &&
                        request.fromBorderWidth > 0) {
                        Color fromBorderColor = { request.fromBorderR, request.fromBorderG, request.fromBorderB, 1.0f };
                        RT_RenderGameBorder(request.animatedX, request.animatedY, request.animatedW, request.animatedH,
                                            request.fromBorderWidth, request.fromBorderRadius, fromBorderColor, request.fullW,
                                            request.fullH, renderVAO, renderVBO);
                    } else if (!request.isRawWindowedMode && request.borderEnabled && request.borderWidth > 0) {
                        Color borderColor = { request.borderR, request.borderG, request.borderB, 1.0f };
                        RT_RenderGameBorder(request.animatedX, request.animatedY, request.animatedW, request.animatedH, request.borderWidth,
                                            request.borderRadius, borderColor, request.fullW, request.fullH, renderVAO, renderVBO);
                    }

                    if (!request.isRawWindowedMode && request.showEyeZoom) {
                        RT_RenderEyeZoom(readyTex, request.eyeZoomAnimatedViewportX, request.fullW, request.fullH, srcW, srcH, renderVAO,
                                         renderVBO, request.isTransitioningFromEyeZoom, request.eyeZoomSnapshotTexture,
                                         request.eyeZoomSnapshotWidth, request.eyeZoomSnapshotHeight, &cfg.eyezoom);
                    }
                }

                // Clean up the game fence (the render thread owns this handle).
                if (request.gameTextureFence && glIsSync(request.gameTextureFence)) { glDeleteSync(request.gameTextureFence); }
            } else {
                // Background/border rendering is done on main thread (render.cpp), we only render overlays here
                glDisable(GL_SCISSOR_TEST);
                glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
                glClear(GL_COLOR_BUFFER_BIT);
            }

            GameViewportGeometry geo;
            geo.gameW = request.gameW;
            geo.gameH = request.gameH;
            if (isObsRequest) {
                geo.finalX = request.animatedX;
                geo.finalY = request.animatedY;
                geo.finalW = request.animatedW;
                geo.finalH = request.animatedH;
            } else {
                geo.finalX = request.finalX;
                geo.finalY = request.finalY;
                geo.finalW = request.finalW;
                geo.finalH = request.finalH;
            }

            // Collect active elements from g_config (this work moved from main thread)
            static const Config* s_cachedActiveCfgPtr = nullptr;
            static std::string s_cachedActiveModeId;
            static bool s_cachedActiveImagesVisible = false;
            static bool s_cachedActiveWindowOverlaysVisible = false;
            static std::vector<MirrorConfig> s_cachedActiveMirrors;
            static std::vector<ImageConfig> s_cachedActiveImages;
            static std::vector<const WindowOverlayConfig*> s_cachedActiveWindowOverlays;

            const bool imagesVisible = g_imageOverlaysVisible.load(std::memory_order_acquire);
            const bool windowOverlaysVisible = g_windowOverlaysVisible.load(std::memory_order_acquire);

            if (s_cachedActiveCfgPtr != &cfg || s_cachedActiveModeId != request.modeId || s_cachedActiveImagesVisible != imagesVisible ||
                s_cachedActiveWindowOverlaysVisible != windowOverlaysVisible) {
                PROFILE_SCOPE_CAT("RT Collect Active Elements", "Render Thread");
                s_cachedActiveCfgPtr = &cfg;
                s_cachedActiveModeId = request.modeId;
                s_cachedActiveImagesVisible = imagesVisible;
                s_cachedActiveWindowOverlaysVisible = windowOverlaysVisible;
                RT_CollectActiveElements(cfg, request.modeId, false, s_cachedActiveMirrors, s_cachedActiveImages, s_cachedActiveWindowOverlays);
            }

            const std::vector<MirrorConfig>& activeMirrors = s_cachedActiveMirrors;
            const std::vector<ImageConfig>& activeImages = s_cachedActiveImages;
            const std::vector<const WindowOverlayConfig*>& activeWindowOverlays = s_cachedActiveWindowOverlays;

            // the render thread doing a full clear + fence every frame. Treat those as "nothing to render".
            const bool excludeOoms = request.excludeOnlyOnMyScreen;
            bool hasVisibleMirrors = false;
            for (const auto& m : activeMirrors) {
                if (excludeOoms && m.onlyOnMyScreen) continue;
                if ((request.overlayOpacity * m.opacity) > 0.0f) {
                    hasVisibleMirrors = true;
                    break;
                }
            }

            bool hasVisibleImages = false;
            for (const auto& img : activeImages) {
                if (excludeOoms && img.onlyOnMyScreen) continue;
                const bool couldHaveVisibleBg = img.background.enabled && img.background.opacity > 0.0f;
                const bool couldHaveVisibleBorder = img.border.enabled && img.border.width > 0;
                if ((request.overlayOpacity * img.opacity) > 0.0f || couldHaveVisibleBg || couldHaveVisibleBorder) {
                    hasVisibleImages = true;
                    break;
                }
            }

            bool hasVisibleWindowOverlays = false;
            {
                if (!activeWindowOverlays.empty()) {
                    for (const WindowOverlayConfig* oconf : activeWindowOverlays) {
                        if (!oconf) continue;
                        if (excludeOoms && oconf->onlyOnMyScreen) continue;
                        const bool couldHaveVisibleBg = oconf->background.enabled && oconf->background.opacity > 0.0f;
                        const bool couldHaveVisibleBorder = oconf->border.enabled && oconf->border.width > 0;
                        if ((request.overlayOpacity * oconf->opacity) > 0.0f || couldHaveVisibleBg || couldHaveVisibleBorder) {
                            hasVisibleWindowOverlays = true;
                            break;
                        }
                    }
                }
            }

            const bool hasAnyVisibleOverlay = hasVisibleMirrors || hasVisibleImages || hasVisibleWindowOverlays;

            bool shouldRenderAnyImGui = request.shouldRenderGui || request.showPerformanceOverlay || request.showProfiler ||
                                        request.showEyeZoom || request.showTextureGrid;

            // Some systems can start the render thread before a valid HWND is published,
            // which previously meant the GUI never initialized (Ctrl+I would do nothing, then ESC could crash).
            if (!g_renderThreadImGuiInitialized && shouldRenderAnyImGui) {
                HWND hwnd = g_minecraftHwnd.load();
                if (hwnd) { RT_TryInitializeImGui(hwnd, cfg); }
            }

            if (!hasAnyVisibleOverlay && !shouldRenderAnyImGui && !request.showWelcomeToast) {
                // Create fence for synchronization
                GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
                glFlush();

                writeFBO.frameNumber = request.frameNumber;

                // Update last good texture and fence atomically
                // The main thread will wait on the fence before using the texture
                if (isObsRequest) {
                    // Exchange fences - delete the OLDEST pending fence, not the one just swapped out
                    GLsync oldFence = g_lastGoodObsFence.exchange(fence, std::memory_order_acq_rel);
                    // Deferred deletion: delete the fence from 2 cycles ago, store current old fence
                    if (g_pendingDeleteObsFences[g_pendingDeleteObsIndex]) {
                        if (glIsSync(g_pendingDeleteObsFences[g_pendingDeleteObsIndex])) {
                            glDeleteSync(g_pendingDeleteObsFences[g_pendingDeleteObsIndex]);
                        }
                    }
                    g_pendingDeleteObsFences[g_pendingDeleteObsIndex] = oldFence;
                    g_pendingDeleteObsIndex = (g_pendingDeleteObsIndex + 1) % FENCE_DELETION_DELAY;
                    g_lastGoodObsTexture.store(writeFBO.texture, std::memory_order_release);
                } else {
                    // Exchange fences - delete the OLDEST pending fence, not the one just swapped out
                    GLsync oldFence = g_lastGoodFence.exchange(fence, std::memory_order_acq_rel);
                    // Deferred deletion: delete the fence from 2 cycles ago, store current old fence
                    if (g_pendingDeleteFences[g_pendingDeleteIndex]) {
                        if (glIsSync(g_pendingDeleteFences[g_pendingDeleteIndex])) { glDeleteSync(g_pendingDeleteFences[g_pendingDeleteIndex]); }
                    }
                    g_pendingDeleteFences[g_pendingDeleteIndex] = oldFence;
                    g_pendingDeleteIndex = (g_pendingDeleteIndex + 1) % FENCE_DELETION_DELAY;
                    g_lastGoodTexture.store(writeFBO.texture, std::memory_order_release);
                }

                if (isObsRequest) {
                    AdvanceObsFBO();
                    {
                        std::lock_guard<std::mutex> lock(g_obsCompletionMutex);
                        g_obsFrameComplete.store(true);
                    }
                    g_obsCompletionCV.notify_one();
                } else {
                    AdvanceWriteFBO();
                    g_renderFrameNumber.store(request.frameNumber);
                    {
                        std::lock_guard<std::mutex> lock(g_completionMutex);
                        g_frameComplete.store(true);
                    }
                    g_completionCV.notify_one();
                }
                continue;
            }


            // This ensures EyeZoom boxes and text are in the same FBO, synchronized
            // Use ready frame texture from mirror thread for synchronized, flicker-free capture
            if (!isObsRequest && request.showEyeZoom) {
                GLuint readyTex = GetReadyGameTexture();
                int srcW = GetReadyGameWidth();
                int srcH = GetReadyGameHeight();

                if (readyTex == 0 || srcW <= 0 || srcH <= 0) {
                    GLuint safeTex = GetSafeReadTexture();
                    if (safeTex != 0) {
                        readyTex = safeTex;
                        srcW = GetFallbackGameWidth();
                        srcH = GetFallbackGameHeight();
                        if (srcW <= 0 || srcH <= 0) {
                            srcW = request.fullW;
                            srcH = request.fullH;
                        }
                    }
                }

                if (readyTex != 0 && srcW > 0 && srcH > 0) {
                    PROFILE_SCOPE_CAT("RT EyeZoom Render", "Render Thread");
                    RT_RenderEyeZoom(readyTex, request.eyeZoomAnimatedViewportX, request.fullW, request.fullH, srcW, srcH, renderVAO,
                                     renderVBO, request.isTransitioningFromEyeZoom, request.eyeZoomSnapshotTexture,
                                     request.eyeZoomSnapshotWidth, request.eyeZoomSnapshotHeight, &cfg.eyezoom);
                }
            }

            if (!request.isRawWindowedMode && !activeMirrors.empty()) {
                PROFILE_SCOPE_CAT("RT Mirror Render", "Render Thread");
                // Swap ready buffers from capture thread (done on render thread to avoid main thread locks)
                // This must happen before reading mirror textures
                SwapMirrorBuffers();

                bool isEyeZoomMode = (request.modeId == "EyeZoom");

                RT_RenderMirrors(activeMirrors, geo, request.fullW, request.fullH, request.overlayOpacity, excludeOoms,
                                 request.relativeStretching, request.transitionProgress, request.mirrorSlideProgress, request.fromX,
                                 request.fromY, request.fromW, request.fromH, request.toX, request.toY, request.toW, request.toH,
                                 isEyeZoomMode, request.isTransitioningFromEyeZoom, request.eyeZoomAnimatedViewportX, request.skipAnimation,
                                 request.fromModeId, request.fromSlideMirrorsIn, request.toSlideMirrorsIn, false /* isSlideOutPass */,
                                 renderVAO, renderVBO);
            }

            if (!request.isRawWindowedMode && request.isTransitioningFromEyeZoom && cfg.eyezoom.slideMirrorsIn && !request.skipAnimation) {
                PROFILE_SCOPE_CAT("RT EyeZoom Mirror Slide Out", "Render Thread");

                std::vector<MirrorConfig> eyeZoomMirrors;
                std::vector<ImageConfig> unusedImages;
                std::vector<const WindowOverlayConfig*> unusedOverlays;
                RT_CollectActiveElements(cfg, "EyeZoom", false, eyeZoomMirrors, unusedImages, unusedOverlays);

                std::vector<MirrorConfig> mirrorsToSlideOut;
                for (const auto& ezMirror : eyeZoomMirrors) {
                    bool existsInTarget = false;
                    for (const auto& targetMirror : activeMirrors) {
                        if (targetMirror.name == ezMirror.name) {
                            existsInTarget = true;
                            break;
                        }
                    }
                    if (!existsInTarget) { mirrorsToSlideOut.push_back(ezMirror); }
                }

                if (!mirrorsToSlideOut.empty()) {
                    RT_RenderMirrors(mirrorsToSlideOut, geo, request.fullW, request.fullH, request.overlayOpacity, excludeOoms,
                                     request.relativeStretching, request.transitionProgress, request.mirrorSlideProgress, request.fromX,
                                     request.fromY, request.fromW, request.fromH, request.toX, request.toY, request.toW, request.toH, true,
                                     request.isTransitioningFromEyeZoom, request.eyeZoomAnimatedViewportX, request.skipAnimation,
                                     request.modeId, cfg.eyezoom.slideMirrorsIn, request.toSlideMirrorsIn, true /* isSlideOutPass */,
                                     renderVAO, renderVBO);
                }
            }

            if (!request.isTransitioningFromEyeZoom && request.fromSlideMirrorsIn && !request.fromModeId.empty() &&
                request.mirrorSlideProgress < 1.0f && !request.skipAnimation) {
                PROFILE_SCOPE_CAT("RT Generic Mirror Slide Out", "Render Thread");

                std::vector<MirrorConfig> fromModeMirrors;
                std::vector<ImageConfig> unusedImages;
                std::vector<const WindowOverlayConfig*> unusedOverlays;
                RT_CollectActiveElements(cfg, request.fromModeId, false, fromModeMirrors, unusedImages, unusedOverlays);

                std::vector<MirrorConfig> mirrorsToSlideOut;
                for (const auto& fromMirror : fromModeMirrors) {
                    bool existsInTarget = false;
                    for (const auto& targetMirror : activeMirrors) {
                        if (targetMirror.name == fromMirror.name) {
                            existsInTarget = true;
                            break;
                        }
                    }
                    if (!existsInTarget) { mirrorsToSlideOut.push_back(fromMirror); }
                }

                if (!mirrorsToSlideOut.empty()) {
                    RT_RenderMirrors(mirrorsToSlideOut, geo, request.fullW, request.fullH, request.overlayOpacity, excludeOoms,
                                     request.relativeStretching, request.transitionProgress, request.mirrorSlideProgress, request.fromX,
                                     request.fromY, request.fromW, request.fromH, request.toX, request.toY, request.toW, request.toH, false,
                                     false, -1, request.skipAnimation, request.modeId, request.fromSlideMirrorsIn, request.toSlideMirrorsIn,
                                     true /* isSlideOutPass */, renderVAO, renderVBO);
                }
            }

            if (!request.isRawWindowedMode && !activeImages.empty()) {
                PROFILE_SCOPE_CAT("RT Image Render", "Render Thread");
                RT_RenderImages(activeImages, request.fullW, request.fullH, request.toX, request.toY, request.toW, request.toH,
                                request.gameW, request.gameH, request.relativeStretching, request.transitionProgress, request.fromX,
                                request.fromY, request.fromW, request.fromH, request.overlayOpacity, excludeOoms, renderVAO, renderVBO);
            }

            if (!activeWindowOverlays.empty()) {
                PROFILE_SCOPE_CAT("RT Window Overlay Render", "Render Thread");
                RT_RenderWindowOverlays(activeWindowOverlays, request.fullW, request.fullH, request.toX, request.toY, request.toW,
                                        request.toH, request.gameW, request.gameH, request.relativeStretching, request.transitionProgress,
                                        request.fromX, request.fromY, request.fromW, request.fromH, request.overlayOpacity, excludeOoms,
                                        renderVAO, renderVBO);
            }

            if (g_renderThreadImGuiInitialized && shouldRenderAnyImGui) {
                PROFILE_SCOPE_CAT("RT ImGui Render", "Render Thread");

                ImGui::SetCurrentContext(g_renderThreadImGuiContext);

                if (g_eyeZoomFontNeedsReload.exchange(false)) {
                    std::string newFontPath = cfg.eyezoom.textFontPath.empty() ? cfg.fontPath : cfg.eyezoom.textFontPath;

                    if (newFontPath != g_eyeZoomFontPathCached) {
                        Log("Render Thread: Reloading EyeZoom font from " + newFontPath);
                        ImGuiIO& io = ImGui::GetIO();

                        g_fontsValid = false;

                        io.Fonts->Clear();

                        (void)RT_AddFontWithArialFallback(io.Fonts, cfg.fontPath, 16.0f * g_eyeZoomScaleFactor, "base font");

                        g_eyeZoomTextFont = RT_AddFontWithArialFallback(io.Fonts, newFontPath, 80.0f * g_eyeZoomScaleFactor, "EyeZoom font",
                                                                        &g_eyeZoomFontPathCached);
                        if (g_eyeZoomFontPathCached.empty()) { g_eyeZoomFontPathCached = ConfigDefaults::CONFIG_FONT_PATH; }

                        InitializeOverlayTextFont(cfg.fontPath.empty() ? ConfigDefaults::CONFIG_FONT_PATH : cfg.fontPath, 16.0f,
                                                  g_eyeZoomScaleFactor);

                        if (!io.Fonts->Build()) {
                            Log("Render Thread: Font atlas build failed after reload; forcing Arial fallback");
                            io.Fonts->Clear();
                            (void)RT_AddFontWithArialFallback(io.Fonts, ConfigDefaults::CONFIG_FONT_PATH, 16.0f * g_eyeZoomScaleFactor,
                                                             "base font (forced Arial)");
                            g_eyeZoomTextFont = RT_AddFontWithArialFallback(io.Fonts, ConfigDefaults::CONFIG_FONT_PATH,
                                                                            80.0f * g_eyeZoomScaleFactor, "EyeZoom font (forced Arial)");
                            InitializeOverlayTextFont(ConfigDefaults::CONFIG_FONT_PATH, 16.0f, g_eyeZoomScaleFactor);
                            if (!io.Fonts->Build()) {
                                Log("ERROR: Render Thread: Font atlas still failing after Arial fallback; using ImGui default font only");
                                io.Fonts->Clear();
                                io.Fonts->AddFontDefault();
                                (void)io.Fonts->Build();
                                g_eyeZoomTextFont = ImGui::GetFont();
                            }
                        }

                        ImGui_ImplOpenGL3_DestroyFontsTexture();
                        ImGui_ImplOpenGL3_CreateFontsTexture();

                        if ((GLuint)(intptr_t)io.Fonts->TexID == 0) {
                            Log("ERROR: Render Thread: ImGui font texture ID is 0 after reload; GUI may render black");
                        }

                        g_fontsValid = true;
                        Log("Render Thread: Fonts reloaded successfully");
                    }
                }

                // If so, reinitialize ImGui Win32 backend with the new HWND
                if (g_hwndChanged.exchange(false)) {
                    HWND newHwnd = g_minecraftHwnd.load();
                    if (newHwnd != NULL) {
                        Log("Render Thread: HWND changed, reinitializing ImGui Win32 backend");
                        ImGui_ImplWin32_Shutdown();
                        ImGui_ImplWin32_Init(newHwnd);
                    }
                }

                ImGui_ImplOpenGL3_NewFrame();
                ImGui_ImplWin32_NewFrame();

                // Feed queued input from the Win32 message thread into ImGui.
                // Must happen before ImGui::NewFrame() to affect the current frame.
                ImGuiInputQueue_DrainToImGui();
                ImGui::NewFrame();

                if (request.showTextureGrid) {
                    RenderTextureGridOverlay(true, request.textureGridModeWidth, request.textureGridModeHeight);
                }

                // so they stay synchronized during transitions
                if (request.showEyeZoom && request.eyeZoomFadeOpacity > 0.0f && g_fontsValid) {
                    EyeZoomConfig zoomConfig = cfg.eyezoom;

                    int modeWidth = zoomConfig.windowWidth;
                    int targetViewportX = (request.fullW - modeWidth) / 2;

                    int viewportX = (request.eyeZoomAnimatedViewportX >= 0) ? request.eyeZoomAnimatedViewportX : targetViewportX;

                    // Calculate dimensions and position - must match RT_RenderEyeZoom logic
                    int zoomOutputWidth;
                    // Use request value, NOT global atomic - ensures text and boxes use identical transition state
                    bool isTransitioningFromEyeZoom = request.isTransitioningFromEyeZoom;
                    bool isTransitioningToEyeZoom = (viewportX < targetViewportX && !isTransitioningFromEyeZoom);

                    int zoomOutputHeight = 0;
                    int finalZoomX = 0;
                    int zoomY = 0;

                    if (zoomConfig.useCustomSizePosition) {
                        zoomOutputWidth = zoomConfig.zoomAreaWidth;
                        zoomOutputHeight = zoomConfig.zoomAreaHeight;
                        finalZoomX = zoomConfig.positionX;
                        zoomY = zoomConfig.positionY;
                    } else {
                        int autoHorizontalMargin = 0;
                        if (targetViewportX > 0) autoHorizontalMargin = targetViewportX / 10;

                        zoomOutputWidth = targetViewportX - (2 * autoHorizontalMargin);
                        int autoVerticalMargin = request.fullH / 8;
                        zoomOutputHeight = request.fullH - (2 * autoVerticalMargin);

                        finalZoomX = autoHorizontalMargin;
                        zoomY = (request.fullH - zoomOutputHeight) / 2;
                    }

                    if (zoomOutputWidth > request.fullW) zoomOutputWidth = request.fullW;
                    if (zoomOutputWidth <= 1) zoomOutputWidth = 0;

                    if (zoomOutputWidth > 20) {

                        int zoomX = finalZoomX;

                        if (zoomConfig.slideZoomIn) {
                            int offScreenX = -zoomOutputWidth;

                            if ((isTransitioningToEyeZoom || isTransitioningFromEyeZoom) && targetViewportX > 0) {
                                float progress = (float)viewportX / (float)targetViewportX;
                                zoomX = offScreenX + (int)((finalZoomX - offScreenX) * progress);
                            }
                        }

                        if (zoomOutputHeight > request.fullH) zoomOutputHeight = request.fullH;
                        if (zoomOutputHeight < 1) zoomOutputHeight = 1;

                        int maxZoomX = (std::max)(0, request.fullW - zoomOutputWidth);
                        int maxZoomY = (std::max)(0, request.fullH - zoomOutputHeight);

                        bool isSlidingNow = zoomConfig.slideZoomIn && (isTransitioningToEyeZoom || isTransitioningFromEyeZoom);
                        if (!isSlidingNow) {
                            zoomX = (std::max)(0, (std::min)(zoomX, maxZoomX));
                        }
                        zoomY = (std::max)(0, (std::min)(zoomY, maxZoomY));

                        float pixelWidthOnScreen = zoomOutputWidth / (float)zoomConfig.cloneWidth;
                        int labelsPerSide = zoomConfig.cloneWidth / 2;
                        int overlayLabelsPerSide = zoomConfig.overlayWidth;
                        if (overlayLabelsPerSide < 0) overlayLabelsPerSide = labelsPerSide;
                        if (overlayLabelsPerSide > labelsPerSide) overlayLabelsPerSide = labelsPerSide;
                        float centerY = zoomY + zoomOutputHeight / 2.0f;

                        ImDrawList* drawList = request.shouldRenderGui ? ImGui::GetBackgroundDrawList() : ImGui::GetForegroundDrawList();
                        float requestedFontSize = (float)zoomConfig.textFontSize;
                        if (requestedFontSize < 1.0f) requestedFontSize = 1.0f;

                        float fontSize = requestedFontSize;
                        if (zoomConfig.autoFontSize) {
                            float boxHeight = zoomConfig.linkRectToFont ? (requestedFontSize * 1.2f) : (float)zoomConfig.rectHeight;
                            float maxFontByWidth = pixelWidthOnScreen * 0.90f;
                            float maxFontByHeight = boxHeight * 0.85f;
                            if (maxFontByWidth > 0.0f) fontSize = (std::min)(fontSize, maxFontByWidth);
                            if (maxFontByHeight > 0.0f) fontSize = (std::min)(fontSize, maxFontByHeight);
                            if (fontSize < 6.0f) fontSize = 6.0f;
                        }
                        float finalTextAlpha = zoomConfig.textColorOpacity * request.eyeZoomFadeOpacity;
                        ImU32 textColor =
                            IM_COL32(static_cast<int>(zoomConfig.textColor.r * 255), static_cast<int>(zoomConfig.textColor.g * 255),
                                     static_cast<int>(zoomConfig.textColor.b * 255), static_cast<int>(finalTextAlpha * 255));

                        if (zoomConfig.activeOverlayIndex < 0 ||
                            zoomConfig.activeOverlayIndex >= (int)zoomConfig.overlays.size()) {
                            ImFont* font = g_eyeZoomTextFont ? g_eyeZoomTextFont : ImGui::GetFont();

                            for (int xOffset = -overlayLabelsPerSide; xOffset <= overlayLabelsPerSide; xOffset++) {
                                if (xOffset == 0) continue;

                                int boxIndex = xOffset + labelsPerSide - (xOffset > 0 ? 1 : 0);
                                float boxLeft = zoomX + (boxIndex * pixelWidthOnScreen);

                                int displayNumber = abs(xOffset);
                                std::string text = std::to_string(displayNumber);

                                float finalFontSize = fontSize;
                                ImVec2 textSize = font->CalcTextSizeA(finalFontSize, FLT_MAX, 0.0f, text.c_str());
                                if (zoomConfig.autoFontSize) {
                                    float maxTextWidth = pixelWidthOnScreen * 0.94f;
                                    if (maxTextWidth > 0.0f && textSize.x > maxTextWidth && textSize.x > 0.0f) {
                                        float scale = maxTextWidth / textSize.x;
                                        finalFontSize = (std::max)(6.0f, finalFontSize * scale);
                                        textSize = font->CalcTextSizeA(finalFontSize, FLT_MAX, 0.0f, text.c_str());
                                    }
                                }
                                float numberCenterX = boxLeft + pixelWidthOnScreen / 2.0f;
                                float numberCenterY = centerY;
                                ImVec2 textPos(numberCenterX - textSize.x / 2.0f, numberCenterY - textSize.y / 2.0f);

                                drawList->AddText(font, finalFontSize, textPos, textColor, text.c_str());
                            }
                        }
                    }
                }

                RenderCachedTextureGridLabels();

                if (request.shouldRenderGui) { RenderSettingsGUI(); }

                RenderPerformanceOverlay(request.showPerformanceOverlay);

                RenderProfilerOverlay(request.showProfiler, request.showPerformanceOverlay);

                // Publish capture flags for the window thread (ESC handling, overlay keyboard forwarding, etc.)
                ImGuiInputQueue_PublishCaptureState();

                ImGui::Render();
                ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            }

            if (request.showWelcomeToast) { RenderWelcomeToast(request.welcomeToastIsFullscreen); }

            // Create fence to signal when GPU completes all rendering commands
            GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);

            glFlush();

            writeFBO.frameNumber = request.frameNumber;

            // Update last good texture and fence atomically
            // The main thread will wait on the fence before using the texture
            if (isObsRequest) {
                // Exchange fences - delete the OLDEST pending fence, not the one just swapped out
                GLsync oldFence = g_lastGoodObsFence.exchange(fence, std::memory_order_acq_rel);
                // Deferred deletion: delete the fence from 2 cycles ago, store current old fence
                if (g_pendingDeleteObsFences[g_pendingDeleteObsIndex]) {
                    if (glIsSync(g_pendingDeleteObsFences[g_pendingDeleteObsIndex])) {
                        glDeleteSync(g_pendingDeleteObsFences[g_pendingDeleteObsIndex]);
                    }
                }
                g_pendingDeleteObsFences[g_pendingDeleteObsIndex] = oldFence;
                g_pendingDeleteObsIndex = (g_pendingDeleteObsIndex + 1) % FENCE_DELETION_DELAY;
                g_lastGoodObsTexture.store(writeFBO.texture, std::memory_order_release);

                if (IsVirtualCameraActive()) {
                    int vcW = request.fullW;
                    int vcH = request.fullH;

                    if (g_vcCursorFBO == 0 || g_vcCursorWidth != vcW || g_vcCursorHeight != vcH) {
                        if (g_vcCursorTexture != 0) { glDeleteTextures(1, &g_vcCursorTexture); }
                        if (g_vcCursorFBO == 0) { glGenFramebuffers(1, &g_vcCursorFBO); }

                        glGenTextures(1, &g_vcCursorTexture);
                        glBindTexture(GL_TEXTURE_2D, g_vcCursorTexture);
                        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, vcW, vcH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                        glBindTexture(GL_TEXTURE_2D, 0);

                        glBindFramebuffer(GL_FRAMEBUFFER, g_vcCursorFBO);
                        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_vcCursorTexture, 0);
                        glBindFramebuffer(GL_FRAMEBUFFER, 0);

                        g_vcCursorWidth = vcW;
                        g_vcCursorHeight = vcH;
                    }

                    glBindFramebuffer(GL_READ_FRAMEBUFFER, writeFBO.fbo);
                    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_vcCursorFBO);
                    glBlitFramebuffer(0, 0, vcW, vcH, 0, 0, vcW, vcH, GL_COLOR_BUFFER_BIT, GL_NEAREST);

                    glBindFramebuffer(GL_FRAMEBUFFER, g_vcCursorFBO);
                    if (oglViewport)
                        oglViewport(0, 0, vcW, vcH);
                    else
                        glViewport(0, 0, vcW, vcH);

                    int viewportX = request.isWindowed ? request.animatedX : 0;
                    int viewportY = request.isWindowed ? request.animatedY : 0;
                    int viewportW = request.isWindowed ? request.animatedW : vcW;
                    int viewportH = request.isWindowed ? request.animatedH : vcH;
                    int windowW = request.isWindowed ? request.windowW : vcW;
                    int windowH = request.isWindowed ? request.windowH : vcH;

                    RT_RenderCursorForObs(vcW, vcH, viewportX, viewportY, viewportW, viewportH, windowW, windowH, renderVAO, renderVBO);

                    glBindFramebuffer(GL_FRAMEBUFFER, writeFBO.fbo);

                    StartVirtualCameraAsyncReadback(g_vcCursorTexture, vcW, vcH);
                } else {
                }
            } else {
                // Exchange fences - delete the OLDEST pending fence, not the one just swapped out
                GLsync oldFence = g_lastGoodFence.exchange(fence, std::memory_order_acq_rel);
                // Deferred deletion: delete the fence from 2 cycles ago, store current old fence
                if (g_pendingDeleteFences[g_pendingDeleteIndex]) {
                    if (glIsSync(g_pendingDeleteFences[g_pendingDeleteIndex])) { glDeleteSync(g_pendingDeleteFences[g_pendingDeleteIndex]); }
                }
                g_pendingDeleteFences[g_pendingDeleteIndex] = oldFence;
                g_pendingDeleteIndex = (g_pendingDeleteIndex + 1) % FENCE_DELETION_DELAY;
                g_lastGoodTexture.store(writeFBO.texture, std::memory_order_release);

            }

            if (isObsRequest) {
                AdvanceObsFBO();
                {
                    std::lock_guard<std::mutex> lock(g_obsCompletionMutex);
                    g_obsFrameComplete.store(true);
                }
                g_obsCompletionCV.notify_one();
            } else {
                AdvanceWriteFBO();
                g_renderFrameNumber.store(request.frameNumber);
                {
                    std::lock_guard<std::mutex> lock(g_completionMutex);
                    g_frameComplete.store(true);
                }
                g_completionCV.notify_one();
            }

            if (hasPendingMain) {
                request = pendingMainRequest;
                isObsRequest = false;
                hasPendingMain = false;
                goto process_request;
            }

            {
                auto endTime = std::chrono::high_resolution_clock::now();
                double renderTime = std::chrono::duration<double, std::milli>(endTime - startTime).count();
                g_lastRenderTimeMs.store(renderTime);

                double avg = g_avgRenderTimeMs.load();
                g_avgRenderTimeMs.store(avg * 0.95 + renderTime * 0.05);

                g_framesRendered.fetch_add(1);
            }
        }

        Log("Render Thread: Cleaning up...");

        RT_CleanupShaders();
        CleanupRenderFBOs();
        if (renderVAO) glDeleteVertexArrays(1, &renderVAO);
        if (renderVBO) glDeleteBuffers(1, &renderVBO);

        if (g_renderThreadImGuiInitialized) {
            ImGui::SetCurrentContext(g_renderThreadImGuiContext);
            ImGui_ImplOpenGL3_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext(g_renderThreadImGuiContext);
            g_renderThreadImGuiContext = nullptr;
            g_renderThreadImGuiInitialized = false;
            Log("Render Thread: ImGui shutdown complete");
        }

        wglMakeCurrent(NULL, NULL);
        if (g_renderThreadContext) {
            if (!g_renderContextIsShared) { wglDeleteContext(g_renderThreadContext); }
            g_renderThreadContext = NULL;
        }

        g_renderThreadRunning.store(false);
        Log("Render Thread: Stopped");

    } catch (const SE_Exception& e) {
        LogException("RenderThreadFunc (SEH)", e.getCode(), e.getInfo());
        g_renderThreadRunning.store(false);
    } catch (const std::exception& e) {
        LogException("RenderThreadFunc", e);
        g_renderThreadRunning.store(false);
    } catch (...) {
        Log("EXCEPTION in RenderThreadFunc: Unknown exception");
        g_renderThreadRunning.store(false);
    }
}

void StartRenderThread(void* gameGLContext) {
    // If thread is already running, don't start another
    if (g_renderThread.joinable()) {
        if (g_renderThreadRunning.load()) {
            Log("Render Thread: Already running");
            return;
        } else {
            Log("Render Thread: Joining finished thread...");
            g_renderThread.join();

            // If the previous thread exited early (exception), it may not have cleaned up.
            if (!g_renderContextIsShared && g_renderThreadContext) {
                wglDeleteContext(g_renderThreadContext);
                g_renderThreadContext = NULL;
            }
            if (!g_renderContextIsShared) {
                if (g_renderOwnedDCHwnd && g_renderThreadDC) {
                    ReleaseDC(g_renderOwnedDCHwnd, g_renderThreadDC);
                }
                g_renderOwnedDCHwnd = NULL;

                if (g_renderFallbackDummyHwnd && g_renderFallbackDummyDC) {
                    ReleaseDC(g_renderFallbackDummyHwnd, g_renderFallbackDummyDC);
                    g_renderFallbackDummyDC = NULL;
                }
                if (g_renderFallbackDummyHwnd) {
                    DestroyWindow(g_renderFallbackDummyHwnd);
                    g_renderFallbackDummyHwnd = NULL;
                }
                g_renderThreadDC = NULL;
            }
        }
    }

    HGLRC sharedContext = GetSharedRenderContext();
    HDC sharedDC = GetSharedRenderContextDC();

    if (sharedContext && sharedDC) {
        // Use the pre-shared context (GPU sharing enabled for all threads)
        g_renderThreadContext = sharedContext;
        g_renderThreadDC = sharedDC;
        g_renderContextIsShared = true;
        Log("Render Thread: Using pre-shared context (GPU texture sharing enabled)");
    } else {
        g_renderContextIsShared = false;

        HDC gameHdc = wglGetCurrentDC();
        HWND gameHwndForDC = NULL;
        if (!gameHdc) {
            HWND hwnd = g_minecraftHwnd.load();
            if (hwnd) {
                gameHdc = GetDC(hwnd);
                gameHwndForDC = hwnd;
            }
        }

        if (!gameHdc) {
            Log("Render Thread: No DC available");
            return;
        }

        // Prefer a dedicated dummy DC for the worker context to avoid cross-thread HDC issues.
        if (RT_CreateFallbackDummyWindowWithMatchingPixelFormat(gameHdc, L"render", g_renderFallbackDummyHwnd, g_renderFallbackDummyDC) &&
            g_renderFallbackDummyDC) {
            g_renderThreadDC = g_renderFallbackDummyDC;
            // If we had to call GetDC(hwnd) only to query the pixel format, release it now.
            if (gameHwndForDC) {
                ReleaseDC(gameHwndForDC, gameHdc);
                gameHwndForDC = NULL;
            }
            g_renderOwnedDCHwnd = NULL;
        } else {
            // Fall back to using the game HDC (less stable on some drivers).
            g_renderThreadDC = gameHdc;
            g_renderOwnedDCHwnd = gameHwndForDC; // Release on StopRenderThread if non-null
        }

        // Create the render context on main thread
        g_renderThreadContext = wglCreateContext(g_renderThreadDC);
        if (!g_renderThreadContext) {
            Log("Render Thread: Failed to create GL context (error " + std::to_string(GetLastError()) + ")");
            if (g_renderOwnedDCHwnd && g_renderThreadDC) {
                ReleaseDC(g_renderOwnedDCHwnd, g_renderThreadDC);
                g_renderOwnedDCHwnd = NULL;
                g_renderThreadDC = NULL;
            }
            return;
        }

        HDC prevDC = wglGetCurrentDC();
        HGLRC prevRC = wglGetCurrentContext();
        if (prevRC) { wglMakeCurrent(NULL, NULL); }

        if (!wglShareLists((HGLRC)gameGLContext, g_renderThreadContext)) {
            DWORD err1 = GetLastError();
            if (!wglShareLists(g_renderThreadContext, (HGLRC)gameGLContext)) {
                DWORD err2 = GetLastError();
                Log("Render Thread: wglShareLists failed (errors " + std::to_string(err1) + ", " + std::to_string(err2) + ")");
                wglDeleteContext(g_renderThreadContext);
                g_renderThreadContext = NULL;
                if (prevRC && prevDC) { wglMakeCurrent(prevDC, prevRC); }
                return;
            }
        }

        if (prevRC && prevDC) { wglMakeCurrent(prevDC, prevRC); }

        Log("Render Thread: Context created and shared on main thread (fallback mode)");
    }

    g_renderThreadShouldStop.store(false);
    g_renderThreadRunning.store(true);
    g_requestReadySlot.store(-1);
    g_obsReadySlot.store(-1);
    g_frameComplete.store(false);
    g_obsFrameComplete.store(false);
    g_writeFBOIndex.store(0);
    g_readFBOIndex.store(-1);
    g_lastGoodTexture.store(0);
    g_lastGoodObsTexture.store(0);
    g_framesRendered.store(0);
    g_framesDropped.store(0);

    // Clear consumer fences (should already be null, but be safe across hot reloads)
    for (int i = 0; i < RENDER_THREAD_FBO_COUNT; ++i) {
        g_renderFBOConsumerFences[i].store(nullptr, std::memory_order_relaxed);
        g_obsFBOConsumerFences[i].store(nullptr, std::memory_order_relaxed);
    }

    // Start thread
    g_renderThread = std::thread(RenderThreadFunc, gameGLContext);
    LogCategory("init", "Render Thread: Started");
}

void StopRenderThread() {
    if (!g_renderThreadRunning.load() && !g_renderThread.joinable()) { return; }

    Log("Render Thread: Stopping...");
    g_renderThreadShouldStop.store(true);

    // Wake up thread if waiting
    g_requestCV.notify_one();

    if (g_renderThread.joinable()) { g_renderThread.join(); }

    Log("Render Thread: Joined");

    // If the render thread crashed, it may not have reached its normal cleanup path.
    if (!g_renderContextIsShared && g_renderThreadContext) {
        wglDeleteContext(g_renderThreadContext);
        g_renderThreadContext = NULL;
    }

    // If we created a fallback dummy window/DC, destroy it on the main thread after join.
    if (!g_renderContextIsShared) {
        if (g_renderOwnedDCHwnd && g_renderThreadDC) {
            ReleaseDC(g_renderOwnedDCHwnd, g_renderThreadDC);
        }
        g_renderOwnedDCHwnd = NULL;

        if (g_renderFallbackDummyHwnd && g_renderFallbackDummyDC) {
            ReleaseDC(g_renderFallbackDummyHwnd, g_renderFallbackDummyDC);
            g_renderFallbackDummyDC = NULL;
        }
        if (g_renderFallbackDummyHwnd) {
            DestroyWindow(g_renderFallbackDummyHwnd);
            g_renderFallbackDummyHwnd = NULL;
        }

        g_renderThreadDC = NULL;
    }
}

void SubmitFrameForRendering(const FrameRenderRequest& request) {
    // Lock-free submission using double-buffered slots
    // Main thread ALWAYS succeeds - never blocks waiting for render thread

    if (g_requestReadySlot.load(std::memory_order_relaxed) != -1) { g_framesDropped.fetch_add(1, std::memory_order_relaxed); }

    // Write to a slot that is NOT currently being copied by the render thread.
    int writeSlot = g_requestWriteSlot.load(std::memory_order_relaxed);
    int readSlotInUse = g_requestReadSlot.load(std::memory_order_acquire);
    if (writeSlot == readSlotInUse) { writeSlot = 1 - writeSlot; }
    if (writeSlot == readSlotInUse) {
        // Extremely unlikely (would require invalid state), but never risk a data race.
        g_framesDropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_requestSlots[writeSlot] = request;

    // This also tells render thread which slot to read from (the one we just wrote)
    g_requestWriteSlot.store(1 - writeSlot, std::memory_order_relaxed);

    g_requestReadySlot.store(writeSlot, std::memory_order_release);
    g_frameComplete.store(false, std::memory_order_relaxed);

    // Signal the condition variable (brief lock only for CV, not for data protection)
    { std::lock_guard<std::mutex> lock(g_requestSignalMutex); }
    g_requestCV.notify_one();
}

int WaitForRenderComplete(int timeoutMs) {
    std::unique_lock<std::mutex> lock(g_completionMutex);

    bool completed = g_completionCV.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                                             [] { return g_frameComplete.load() || g_renderThreadShouldStop.load(); });

    if (g_renderThreadShouldStop.load()) return -1;
    if (!completed) return -1;

    g_frameComplete.store(false);
    return g_readFBOIndex.load();
}

GLuint GetCompletedRenderTexture() {
    // after the GPU fence wait completes on the render thread
    return g_lastGoodTexture.load(std::memory_order_acquire);
}

GLsync GetCompletedRenderFence() {
    // Return the fence associated with the last good texture
    // This is more efficient than glFinish() as it only waits for the render thread's commands
    return g_lastGoodFence.load(std::memory_order_acquire);
}

CompletedRenderFrame GetCompletedRenderFrame() {
    CompletedRenderFrame out;
    out.texture = g_lastGoodTexture.load(std::memory_order_acquire);
    out.fence = g_lastGoodFence.load(std::memory_order_acquire);
    out.fboIndex = FindFboIndexByTexture(g_renderFBOs, out.texture);
    return out;
}

void SubmitRenderFBOConsumerFence(int fboIndex, GLsync consumerFence) {
    if (!consumerFence) return;
    if (fboIndex < 0 || fboIndex >= RENDER_THREAD_FBO_COUNT) {
        if (glIsSync(consumerFence)) { glDeleteSync(consumerFence); }
        return;
    }

    GLsync old = g_renderFBOConsumerFences[fboIndex].exchange(consumerFence, std::memory_order_acq_rel);
    if (old && glIsSync(old)) { glDeleteSync(old); }
}

void SubmitObsFrameContext(const ObsFrameSubmission& submission) {
    // Lock-free submission using double-buffered slots
    // Main thread ALWAYS succeeds - never blocks waiting for render thread

    // NOTE: We do NOT delete fences here even if overwriting a pending submission.
    // The render thread owns the gameTextureFence and is responsible for deleting it
    // after processing. Deleting here causes a race condition where the render thread
    // may have already copied the fence pointer and will try to delete it again.
    // Occasional fence leaks from dropped frames are acceptable and rare.

    if (g_obsReadySlot.load(std::memory_order_relaxed) != -1) { g_framesDropped.fetch_add(1, std::memory_order_relaxed); }

    // Write to a slot that is NOT currently being copied by the render thread.
    int writeSlot = g_obsWriteSlot.load(std::memory_order_relaxed);
    int readSlotInUse = g_obsReadSlot.load(std::memory_order_acquire);
    if (writeSlot == readSlotInUse) { writeSlot = 1 - writeSlot; }
    if (writeSlot == readSlotInUse) {
        // Avoid data race on ObsFrameSubmission (contains std::string in context).
        g_framesDropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_obsSubmissionSlots[writeSlot] = submission;

    g_obsWriteSlot.store(1 - writeSlot, std::memory_order_relaxed);

    g_obsReadySlot.store(writeSlot, std::memory_order_release);
    g_obsFrameComplete.store(false, std::memory_order_relaxed);

    { std::lock_guard<std::mutex> lock(g_requestSignalMutex); }
    g_requestCV.notify_one();
}

GLuint GetCompletedObsTexture() {
    // after the GPU fence wait completes on the render thread
    return g_lastGoodObsTexture.load(std::memory_order_acquire);
}

GLsync GetCompletedObsFence() {
    // Return the fence associated with the last good OBS texture
    // This is more efficient than glFinish() as it only waits for the render thread's commands
    return g_lastGoodObsFence.load(std::memory_order_acquire);
}

FrameRenderRequest BuildObsFrameRequest(const ObsFrameContext& ctx, bool isDualRenderingPath) {
    static uint64_t s_obsFrameNumber = 0;

    // Use config snapshot for thread-safe access
    auto obsCfgSnap = GetConfigSnapshot();
    if (!obsCfgSnap) return {};
    const Config& obsCfg = *obsCfgSnap;

    ModeTransitionState transitionState = GetModeTransitionState();

    FrameRenderRequest req;
    req.frameNumber = ++s_obsFrameNumber;
    req.fullW = ctx.fullW;
    req.fullH = ctx.fullH;
    req.gameW = ctx.gameW;
    req.gameH = ctx.gameH;
    req.gameTextureId = ctx.gameTextureId;
    req.modeId = ctx.modeId;
    req.overlayOpacity = 1.0f;
    req.obsDetected = true;
    req.excludeOnlyOnMyScreen = true;
    req.skipAnimation = false;
    req.isObsPass = true;
    req.relativeStretching = ctx.relativeStretching;
    req.fromModeId = transitionState.fromModeId;

    if (!transitionState.fromModeId.empty()) {
        const ModeConfig* fromMode = GetModeFromSnapshot(obsCfg, transitionState.fromModeId);
        if (fromMode) { req.fromSlideMirrorsIn = fromMode->slideMirrorsIn; }
    }
    const ModeConfig* toMode = GetModeFromSnapshot(obsCfg, ctx.modeId);
    if (toMode) { req.toSlideMirrorsIn = toMode->slideMirrorsIn; }

    if (transitionState.active && transitionState.moveProgress < 1.0f) {
        req.mirrorSlideProgress = transitionState.moveProgress;
    } else {
        req.mirrorSlideProgress = 1.0f;
    }

    bool transitionEffectivelyComplete = !transitionState.active || transitionState.progress >= 1.0f;

    if (isDualRenderingPath) {
        bool stillAnimating = transitionState.active && transitionState.progress < 1.0f;

        if (stillAnimating) {
            req.isAnimating = true;
            req.finalX = transitionState.targetX;
            req.finalY = transitionState.targetY;
            req.finalW = transitionState.targetWidth;
            req.finalH = transitionState.targetHeight;
            req.animatedX = transitionState.x;
            req.animatedY = transitionState.y;
            req.animatedW = transitionState.width;
            req.animatedH = transitionState.height;

            req.transitionProgress = transitionState.moveProgress;
            req.fromX = transitionState.fromX;
            req.fromY = transitionState.fromY;
            req.fromW = transitionState.fromWidth;
            req.fromH = transitionState.fromHeight;

            // Must match screen behavior: always use target, not animated position
            req.toX = transitionState.targetX;
            req.toY = transitionState.targetY;
            req.toW = transitionState.targetWidth;
            req.toH = transitionState.targetHeight;
        } else {
            req.isAnimating = false;
            ModeViewportInfo viewport = GetCurrentModeViewport();
            int finalX, finalY, finalW, finalH;
            if (viewport.valid) {
                finalX = viewport.stretchX;
                finalY = viewport.stretchY;
                finalW = viewport.stretchWidth;
                finalH = viewport.stretchHeight;
            } else {
                finalX = (ctx.fullW - ctx.gameW) / 2;
                finalY = (ctx.fullH - ctx.gameH) / 2;
                finalW = ctx.gameW;
                finalH = ctx.gameH;
            }
            req.animatedX = finalX;
            req.animatedY = finalY;
            req.animatedW = finalW;
            req.animatedH = finalH;
            req.transitionProgress = 1.0f;
            req.fromX = finalX;
            req.fromY = finalY;
            req.fromW = finalW;
            req.fromH = finalH;
            req.toX = finalX;
            req.toY = finalY;
            req.toW = finalW;
            req.toH = finalH;
            req.finalX = finalX;
            req.finalY = finalY;
            req.finalW = finalW;
            req.finalH = finalH;
        }
    } else {
        if (!transitionEffectivelyComplete) {
            req.isAnimating = true;
            req.animatedX = transitionState.x;
            req.animatedY = transitionState.y;
            req.animatedW = transitionState.width;
            req.animatedH = transitionState.height;
            req.transitionProgress = transitionState.moveProgress;
            req.fromX = transitionState.fromX;
            req.fromY = transitionState.fromY;
            req.fromW = transitionState.fromWidth;
            req.fromH = transitionState.fromHeight;

            bool inBouncePhase = transitionState.moveProgress >= 1.0f;
            if (inBouncePhase) {
                req.toX = transitionState.x;
                req.toY = transitionState.y;
                req.toW = transitionState.width;
                req.toH = transitionState.height;
            } else {
                req.toX = transitionState.targetX;
                req.toY = transitionState.targetY;
                req.toW = transitionState.targetWidth;
                req.toH = transitionState.targetHeight;
            }

            req.finalX = transitionState.targetX;
            req.finalY = transitionState.targetY;
            req.finalW = transitionState.targetWidth;
            req.finalH = transitionState.targetHeight;
        } else {
            req.isAnimating = false;
            ModeViewportInfo viewport = GetCurrentModeViewport();
            int finalX, finalY, finalW, finalH;
            if (viewport.valid) {
                finalX = viewport.stretchX;
                finalY = viewport.stretchY;
                finalW = viewport.stretchWidth;
                finalH = viewport.stretchHeight;
            } else {
                finalX = (ctx.fullW - ctx.gameW) / 2;
                finalY = (ctx.fullH - ctx.gameH) / 2;
                finalW = ctx.gameW;
                finalH = ctx.gameH;
            }
            req.animatedX = finalX;
            req.animatedY = finalY;
            req.animatedW = finalW;
            req.animatedH = finalH;
            req.transitionProgress = 1.0f;
            req.fromX = finalX;
            req.fromY = finalY;
            req.fromW = finalW;
            req.fromH = finalH;
            req.toX = finalX;
            req.toY = finalY;
            req.toW = finalW;
            req.toH = finalH;
            req.finalX = finalX;
            req.finalY = finalY;
            req.finalW = finalW;
            req.finalH = finalH;
        }
    }

    if (ctx.isWindowed && ctx.windowW > 0 && ctx.windowH > 0) {
        int contentW = ctx.windowW;
        int contentH = ctx.windowH;

        req.fullW = contentW;
        req.fullH = contentH;
        req.animatedX = 0;
        req.animatedY = 0;
        req.animatedW = contentW;
        req.animatedH = contentH;
        req.fromX = 0;
        req.fromY = 0;
        req.fromW = contentW;
        req.fromH = contentH;
        req.toX = 0;
        req.toY = 0;
        req.toW = contentW;
        req.toH = contentH;
        req.finalX = 0;
        req.finalY = 0;
        req.finalW = contentW;
        req.finalH = contentH;
        req.gameW = contentW;
        req.gameH = contentH;
        req.isAnimating = false;
        req.transitionProgress = 1.0f;

        req.isWindowed = true;
        req.windowW = ctx.windowW;
        req.windowH = ctx.windowH;
        req.isPre113Windowed = true;
        req.isRawWindowedMode = ctx.isRawWindowedMode;

        req.bgR = 0.0f;
        req.bgG = 0.0f;
        req.bgB = 0.0f;
    }

    bool transitioningToFullscreen = EqualsIgnoreCase(ctx.modeId, "Fullscreen") && !transitionState.fromModeId.empty();
    if (transitioningToFullscreen && !transitionEffectivelyComplete) {
        const ModeConfig* fromMode = GetModeFromSnapshot(obsCfg, transitionState.fromModeId);
        if (fromMode) {
            req.bgR = fromMode->background.color.r;
            req.bgG = fromMode->background.color.g;
            req.bgB = fromMode->background.color.b;
        } else {
            req.bgR = ctx.bgR;
            req.bgG = ctx.bgG;
            req.bgB = ctx.bgB;
        }
    } else {
        req.bgR = ctx.bgR;
        req.bgG = ctx.bgG;
        req.bgB = ctx.bgB;
    }

    const ModeConfig* currentMode = GetModeFromSnapshot(obsCfg, ctx.modeId);
    if (currentMode) {
        req.borderEnabled = currentMode->border.enabled;
        req.borderR = currentMode->border.color.r;
        req.borderG = currentMode->border.color.g;
        req.borderB = currentMode->border.color.b;
        req.borderWidth = currentMode->border.width;
        req.borderRadius = currentMode->border.radius;
    }

    req.transitioningToFullscreen = transitioningToFullscreen && !transitionEffectivelyComplete;
    if (req.transitioningToFullscreen && !transitionState.fromModeId.empty()) {
        const ModeConfig* fromMode = GetModeFromSnapshot(obsCfg, transitionState.fromModeId);
        if (fromMode) {
            req.fromBorderEnabled = fromMode->border.enabled;
            req.fromBorderR = fromMode->border.color.r;
            req.fromBorderG = fromMode->border.color.g;
            req.fromBorderB = fromMode->border.color.b;
            req.fromBorderWidth = fromMode->border.width;
            req.fromBorderRadius = fromMode->border.radius;
        }
    }

    req.shouldRenderGui = ctx.shouldRenderGui;
    req.showPerformanceOverlay = ctx.showPerformanceOverlay;
    req.showProfiler = ctx.showProfiler;
    req.showEyeZoom = ctx.isEyeZoom || ctx.isTransitioningFromEyeZoom;
    req.eyeZoomFadeOpacity = 1.0f;
    req.eyeZoomAnimatedViewportX = isDualRenderingPath ? transitionState.x : ctx.eyeZoomAnimatedViewportX;
    req.isTransitioningFromEyeZoom = ctx.isTransitioningFromEyeZoom;
    req.eyeZoomSnapshotTexture = ctx.eyeZoomSnapshotTexture;
    req.eyeZoomSnapshotWidth = ctx.eyeZoomSnapshotWidth;
    req.eyeZoomSnapshotHeight = ctx.eyeZoomSnapshotHeight;
    req.showTextureGrid = ctx.showTextureGrid;
    req.textureGridModeWidth = ctx.gameW;
    req.textureGridModeHeight = ctx.gameH;

    req.showWelcomeToast = ctx.showWelcomeToast;
    req.welcomeToastIsFullscreen = ctx.welcomeToastIsFullscreen;

    return req;
}


