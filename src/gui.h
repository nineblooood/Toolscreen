#pragma once

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "config_defaults.h"
#include "imgui.h"
#include "version.h"

typedef unsigned int GLuint;

struct Color {
    float r = 0.0f, g = 0.0f, b = 0.0f, a = 1.0f;
};

struct DecodedImageData {
    enum Type { Background, UserImage };
    Type type;
    std::string id;
    int width = 0, height = 0, channels = 0;
    unsigned char* data = nullptr;

    bool isAnimated = false;
    int frameCount = 0;
    int frameHeight = 0;
    std::vector<int> frameDelays;
};

void ParseColorString(const std::string& input, Color& outColor);
DWORD StringToVk(const std::string& keyStr);
std::string VkToString(DWORD vk);
ImGuiKey VkToImGuiKey(int vk);
void WriteCurrentModeToFile(const std::string& modeId);
void LoadImageAsync(DecodedImageData::Type type, std::string id, std::string path, const std::wstring& toolscreenPath);
std::string WideToUtf8(const std::wstring& wide_string);
void HandleImGuiContextReset();
void InitializeImGuiContext(HWND hwnd);
void StartSupportersFetch();
bool IsGuiHotkeyPressed(WPARAM wParam);
bool IsHotkeyBindingActive();
bool IsRebindBindingActive();
void ResetTransientBindingUiState();
void MarkRebindBindingActive();
void MarkHotkeyBindingActive();
std::vector<DWORD> ParseHotkeyString(const std::string& hotkeyStr);
void RegisterBindingInputEvent(UINT uMsg, WPARAM wParam, LPARAM lParam);
uint64_t GetLatestBindingInputSequence();
bool ConsumeBindingInputEventSince(uint64_t& lastSeenSequence, DWORD& outVk, LPARAM& outLParam, bool& outIsMouseButton);

extern ImFont* g_keyboardLayoutPrimaryFont;
extern ImFont* g_keyboardLayoutSecondaryFont;

enum class GradientAnimationType {
    None,
    Rotate,
    Slide,
    Wave,
    Spiral,
    Fade
};

struct GradientColorStop {
    Color color = { 0.0f, 0.0f, 0.0f };
    float position = 0.0f;
};

struct BackgroundConfig {
    std::string selectedMode = "color";
    std::string image;
    Color color;

    std::vector<GradientColorStop> gradientStops;
    float gradientAngle = 0.0f;

    GradientAnimationType gradientAnimation = GradientAnimationType::None;
    float gradientAnimationSpeed = 1.0f;
    bool gradientColorFade = false;
};

struct MirrorCaptureConfig {
    int x = 0, y = 0;
    std::string relativeTo = "topLeftScreen";
};
struct MirrorRenderConfig {
    int x = 0, y = 0;
    bool useRelativePosition = false;
    float relativeX = 0.5f;
    float relativeY = 0.5f;
    float scale = 1.0f;
    bool separateScale = false;
    float scaleX = 1.0f;
    float scaleY = 1.0f;
    std::string relativeTo = "topLeftScreen";
};
struct MirrorColors {
    std::vector<Color> targetColors;
    Color output, border;
};

enum class MirrorGammaMode {
    Auto = 0,
    AssumeSRGB = 1,
    AssumeLinear = 2
};

enum class MirrorBorderType {
    Dynamic,
    Static
};

enum class MirrorBorderShape {
    Rectangle,
    Circle
};

enum class HookChainingNextTarget {
    LatestHook = 0,
    OriginalFunction = 1,
};

struct MirrorBorderConfig {
    MirrorBorderType type = MirrorBorderType::Dynamic;

    int dynamicThickness = 1;

    MirrorBorderShape staticShape = MirrorBorderShape::Rectangle;
    Color staticColor = { 1.0f, 1.0f, 1.0f };
    int staticThickness = 2;
    int staticRadius = 0;
    int staticOffsetX = 0;
    int staticOffsetY = 0;
    int staticWidth = 0;
    int staticHeight = 0;
};

struct MirrorConfig {
    std::string name;
    int captureWidth = 50;
    int captureHeight = 50;
    std::vector<MirrorCaptureConfig> input;
    MirrorRenderConfig output;
    MirrorColors colors;
    float colorSensitivity = 0.001f;
    MirrorBorderConfig border;
    int fps = 30;
    float opacity = 1.0f;
    bool rawOutput = false;
    bool colorPassthrough = false;
    bool onlyOnMyScreen = false;
};
struct MirrorGroupItem {
    std::string mirrorId;
    bool enabled = true;
    float widthPercent = 1.0f;
    float heightPercent = 1.0f;
    int offsetX = 0;
    int offsetY = 0;
};
struct MirrorGroupConfig {
    std::string name;
    MirrorRenderConfig output;
    std::vector<MirrorGroupItem> mirrors;
};
struct ImageBackgroundConfig {
    bool enabled = false;
    Color color = { 0.0f, 0.0f, 0.0f };
    float opacity = 1.0f;
};
struct StretchConfig {
    bool enabled = false;
    int width = 0, height = 0, x = 0, y = 0;

    std::string widthExpr;
    std::string heightExpr;
    std::string xExpr;
    std::string yExpr;
};
struct BorderConfig {
    bool enabled = false;
    Color color = { 1.0f, 1.0f, 1.0f };
    int width = 4;
    int radius = 0;
};
struct ColorKeyConfig {
    Color color;
    float sensitivity = 0.05f;
};
struct ImageConfig {
    std::string name;
    std::string path;
    int x = 0, y = 0;
    float scale = 1.0f;
    std::string relativeTo = "topLeftScreen";
    int crop_top = 0, crop_bottom = 0, crop_left = 0, crop_right = 0;
    bool enableColorKey = false;
    std::vector<ColorKeyConfig> colorKeys;
    Color colorKey;
    float colorKeySensitivity = 0.001f;
    float opacity = 1.0f;
    ImageBackgroundConfig background;
    bool pixelatedScaling = false;
    bool onlyOnMyScreen = false;
    BorderConfig border;
};
struct WindowOverlayConfig {
    std::string name;
    std::string windowTitle;
    std::string windowClass;
    std::string executableName;
    std::string windowMatchPriority = "title";
    int x = 0, y = 0;
    float scale = 1.0f;
    std::string relativeTo = "topLeftScreen";
    int crop_top = 0, crop_bottom = 0, crop_left = 0, crop_right = 0;
    bool enableColorKey = false;
    std::vector<ColorKeyConfig> colorKeys;
    Color colorKey;
    float colorKeySensitivity = 0.001f;
    float opacity = 1.0f;
    ImageBackgroundConfig background;
    bool pixelatedScaling = false;
    bool onlyOnMyScreen = false;
    int fps = 30;
    int searchInterval = 1000;
    std::string captureMethod = "Windows 10+"; // Capture method: "Windows 10+" (default) or "BitBlt"
    bool enableInteraction = false;
    BorderConfig border;
};

enum class GameTransitionType {
    Cut,
    Bounce
};

enum class OverlayTransitionType {
    Cut
};

enum class BackgroundTransitionType {
    Cut
};

enum class EasingType {
    Linear,
    EaseOut,
    EaseIn,
    EaseInOut
};

struct ModeConfig {
    std::string id;
    int width = 0, height = 0;
    bool useRelativeSize = false;
    float relativeWidth = 0.5f;
    float relativeHeight = 0.5f;

    std::string widthExpr;
    std::string heightExpr;

    BackgroundConfig background;
    std::vector<std::string> mirrorIds;
    std::vector<std::string> mirrorGroupIds;
    std::vector<std::string> imageIds;
    std::vector<std::string> windowOverlayIds;
    StretchConfig stretch;

    GameTransitionType gameTransition = GameTransitionType::Bounce;
    OverlayTransitionType overlayTransition = OverlayTransitionType::Cut;
    BackgroundTransitionType backgroundTransition = BackgroundTransitionType::Cut;
    int transitionDurationMs = 500;

    float easeInPower = 1.0f;
    float easeOutPower = 3.0f;
    int bounceCount = 0;
    float bounceIntensity = 0.15f;
    int bounceDurationMs = 150;
    bool relativeStretching = false;
    bool skipAnimateX = false;
    bool skipAnimateY = false;

    BorderConfig border;

    bool sensitivityOverrideEnabled = false;
    float modeSensitivity = 1.0f;
    bool separateXYSensitivity = false;
    float modeSensitivityX = 1.0f;
    float modeSensitivityY = 1.0f;

    bool slideMirrorsIn = false;
};
struct HotkeyConditions {
    std::vector<std::string> gameState;
    std::vector<DWORD> exclusions;
};
struct AltSecondaryMode {
    std::vector<DWORD> keys;
    std::string mode;
};
struct HotkeyConfig {
    std::vector<DWORD> keys;

    std::string mainMode;
    std::string secondaryMode;
    std::vector<AltSecondaryMode> altSecondaryModes;

    HotkeyConditions conditions;
    int debounce = 100;
    bool triggerOnRelease = false; // When true, hotkey triggers on key release instead of key press

    bool blockKeyFromGame = false;

    bool allowExitToFullscreenRegardlessOfGameState = false;
};

struct SensitivityHotkeyConfig {
    std::vector<DWORD> keys;
    float sensitivity = 1.0f;
    bool separateXY = false;
    float sensitivityX = 1.0f;
    float sensitivityY = 1.0f;
    bool toggle = false;
    HotkeyConditions conditions;
    int debounce = 100;
};
struct DebugGlobalConfig {
    bool showPerformanceOverlay = false;
    bool showProfiler = false;
    float profilerScale = 0.8f;
    bool showHotkeyDebug = false;
    bool fakeCursor = false;
    bool showTextureGrid = false;
    bool delayRenderingUntilFinished = false;
    bool delayRenderingUntilBlitted = false;  // Wait on async overlay blit fence before SwapBuffers
    bool virtualCameraEnabled = false;        // Output to OBS Virtual Camera driver
    int virtualCameraFps = 60;

    bool logModeSwitch = false;
    bool logAnimation = false;
    bool logHotkey = false;
    bool logObs = false;
    bool logWindowOverlay = false;
    bool logFileMonitor = false;
    bool logImageMonitor = false;
    bool logPerformance = false;
    bool logTextureOps = false;
    bool logGui = false;
    bool logInit = false;
    bool logCursorTextures = false;
};
struct CursorConfig {
    std::string cursorName = "";
    int cursorSize = 64;
};
struct CursorsConfig {
    bool enabled = false;
    CursorConfig title;
    CursorConfig wall;
    CursorConfig ingame;
};
enum class EyeZoomOverlayDisplayMode { Manual, Fit, Stretch };

struct EyeZoomOverlayConfig {
    std::string name;
    std::string path;
    EyeZoomOverlayDisplayMode displayMode = EyeZoomOverlayDisplayMode::Fit;
    int manualWidth = 100;
    int manualHeight = 100;
    float opacity = 1.0f;
};

struct EyeZoomConfig {
    int cloneWidth = 24;
    int overlayWidth = 12;
    int cloneHeight = 2080;
    int stretchWidth = 810;
    int windowWidth = 384;
    int windowHeight = 16384;
    int zoomAreaWidth = 0;
    int zoomAreaHeight = 0;
    bool useCustomSizePosition = false;
    int positionX = 0;
    int positionY = 0;
    bool autoFontSize = true;
    int textFontSize = 24;
    std::string textFontPath;
    int rectHeight = 24;
    bool linkRectToFont = true;
    Color gridColor1 = { 1.0f, 0.714f, 0.757f };
    float gridColor1Opacity = 1.0f;
    Color gridColor2 = { 0.678f, 0.847f, 0.902f };
    float gridColor2Opacity = 1.0f;
    Color centerLineColor = { 1.0f, 1.0f, 1.0f };
    float centerLineColorOpacity = 1.0f;
    Color textColor = { 0.0f, 0.0f, 0.0f };
    float textColorOpacity = 1.0f;
    bool slideZoomIn = false;
    bool slideMirrorsIn = false;
    int activeOverlayIndex = -1; // -1 = Default (numbered boxes), 0+ = custom overlay index
    std::vector<EyeZoomOverlayConfig> overlays;
};
struct AppearanceConfig {
    std::string theme = "Dark";
    std::map<std::string, Color> customColors;
};

struct KeyRebind {
    DWORD fromKey = 0;
    DWORD toKey = 0;
    bool enabled = true;

    bool useCustomOutput = false;
    DWORD customOutputVK = 0;
    DWORD customOutputUnicode = 0;
    DWORD customOutputScanCode = 0;
};
struct KeyRebindsConfig {
    bool enabled = false;
    std::vector<KeyRebind> rebinds;
};
struct Config {
    int configVersion = 2;
    std::vector<MirrorConfig> mirrors;
    std::vector<MirrorGroupConfig> mirrorGroups;
    std::vector<ImageConfig> images;
    std::vector<WindowOverlayConfig> windowOverlays;
    std::vector<ModeConfig> modes;
    std::vector<HotkeyConfig> hotkeys;
    std::vector<SensitivityHotkeyConfig> sensitivityHotkeys;
    EyeZoomConfig eyezoom;
    std::string defaultMode = "fullscreen";
    DebugGlobalConfig debug;
    std::vector<DWORD> guiHotkey = ConfigDefaults::GetDefaultGuiHotkey();
    std::vector<DWORD> borderlessHotkey = {};
    bool autoBorderless = false;
    std::vector<DWORD> imageOverlaysHotkey = {};
    std::vector<DWORD> windowOverlaysHotkey = {};
    CursorsConfig cursors;
    std::string fontPath = "c:\\Windows\\Fonts\\Arial.ttf";
    int fpsLimit = 0;
    int fpsLimitSleepThreshold = 1000;
    MirrorGammaMode mirrorGammaMode = MirrorGammaMode::Auto;
    // Useful if a specific overlay/driver hook layer is unstable when chained.
    bool disableHookChaining = false;
    HookChainingNextTarget hookChainingNextTarget = HookChainingNextTarget::OriginalFunction;
    bool allowCursorEscape = false;
    float mouseSensitivity = 1.0f;
    int windowsMouseSpeed = 0;                              // Windows mouse speed override (0 = disabled, 1-20 = override)
    bool hideAnimationsInGame = false;
    KeyRebindsConfig keyRebinds;
    AppearanceConfig appearance;
    int keyRepeatStartDelay = 0;
    int keyRepeatDelay = 0;
    bool basicModeEnabled = false;
    bool disableFullscreenPrompt = false;
    bool disableConfigurePrompt = false;
};
struct GameViewportGeometry {
    int gameW = 0, gameH = 0;
    int finalX = 0, finalY = 0, finalW = 0, finalH = 0;
};

struct ModeTransitionAnimation {
    bool active = false;
    std::chrono::steady_clock::time_point startTime;
    float duration = 0.3f;

    GameTransitionType gameTransition = GameTransitionType::Cut;
    OverlayTransitionType overlayTransition = OverlayTransitionType::Cut;
    BackgroundTransitionType backgroundTransition = BackgroundTransitionType::Cut;

    float easeInPower = 1.0f;
    float easeOutPower = 3.0f;
    int bounceCount = 0;
    float bounceIntensity = 0.15f;
    int bounceDurationMs = 150;
    bool skipAnimateX = false;
    bool skipAnimateY = false;

    std::string fromModeId;
    int fromWidth = 0;
    int fromHeight = 0;
    int fromX = 0;
    int fromY = 0;

    std::string toModeId;
    int toWidth = 0;
    int toHeight = 0;
    int toX = 0;
    int toY = 0;

    int fromNativeWidth = 0;
    int fromNativeHeight = 0;
    int toNativeWidth = 0;
    int toNativeHeight = 0;

    int currentWidth = 0;
    int currentHeight = 0;
    int currentX = 0;
    int currentY = 0;
    float progress = 0.0f;
    float moveProgress = 0.0f;

    int lastSentWidth = 0;
    int lastSentHeight = 0;

    bool wmSizeSent = false;
};

extern Config g_config;
extern std::atomic<bool> g_configIsDirty;

// g_config is the mutable draft, only touched by the GUI/main thread.
// After any mutation, call PublishConfigSnapshot() to atomically publish an
// immutable snapshot. Reader threads call GetConfigSnapshot() to get a
// shared_ptr<const Config> they can safely use without locking.
// Hot-path readers (render thread, logic thread, input hook) grab a snapshot
// once per frame/tick and work from that — zero contention, zero mutex.

// Atomically publish current g_config as an immutable snapshot.
void PublishConfigSnapshot();

// Get the latest published config snapshot. Lock-free, safe from any thread.
std::shared_ptr<const Config> GetConfigSnapshot();

// HOTKEY SECONDARY MODE STATE (separated from Config for thread safety)
// state mutated by input_hook and logic_thread while Config is read elsewhere.
// This separate structure is guarded by its own lightweight mutex.

// Get the current secondary mode for a hotkey by index. Thread-safe.
std::string GetHotkeySecondaryMode(size_t hotkeyIndex);

// Set the current secondary mode for a hotkey by index. Thread-safe.
void SetHotkeySecondaryMode(size_t hotkeyIndex, const std::string& mode);

// Reset all hotkey secondary modes to their config defaults. Thread-safe.
void ResetAllHotkeySecondaryModes();

// Reset all hotkey secondary modes using a specific config snapshot. Thread-safe.
void ResetAllHotkeySecondaryModes(const Config& config);

void ResizeHotkeySecondaryModes(size_t count);

extern std::mutex g_hotkeySecondaryModesMutex;
extern std::atomic<bool> g_cursorsNeedReload;
extern std::atomic<bool> g_showGui;
extern std::atomic<bool> g_imageOverlaysVisible;
extern std::atomic<bool> g_windowOverlaysVisible;
extern std::string g_currentlyEditingMirror;
extern std::atomic<HWND> g_minecraftHwnd;
extern std::wstring g_toolscreenPath;
extern std::string g_currentModeId;
extern std::mutex g_modeIdMutex;
// Lock-free mode ID access (double-buffered)
extern std::string g_modeIdBuffers[2];
extern std::atomic<int> g_currentModeIdIndex;
extern GameVersion g_gameVersion;
extern std::atomic<bool> g_screenshotRequested;
extern std::atomic<bool> g_pendingImageLoad;
extern std::atomic<bool> g_allImagesLoaded;
extern std::atomic<uint64_t> g_configSnapshotVersion;
extern std::string g_configLoadError;
extern std::mutex g_configErrorMutex;
extern std::wstring g_modeFilePath;
extern std::atomic<bool> g_configLoadFailed;
extern std::unordered_map<std::string, std::chrono::steady_clock::time_point> g_hotkeyTimestamps;
extern std::mutex g_hotkeyTimestampsMutex;
extern std::atomic<bool> g_guiNeedsRecenter;
// Lock-free GUI toggle debounce timestamp (milliseconds since epoch)
extern std::atomic<int64_t> g_lastGuiToggleTimeMs;

struct TempSensitivityOverride {
    bool active = false;
    float sensitivityX = 1.0f;
    float sensitivityY = 1.0f;
    int activeSensHotkeyIndex = -1;
};
extern TempSensitivityOverride g_tempSensitivityOverride;
extern std::mutex g_tempSensitivityMutex;

void ClearTempSensitivityOverride();

extern ModeTransitionAnimation g_modeTransition;
extern std::mutex g_modeTransitionMutex;
extern std::atomic<bool> g_skipViewportAnimation;
extern std::atomic<int> g_wmMouseMoveCount;

// This is a compact snapshot updated atomically for lock-free reads
struct ViewportTransitionSnapshot {
    bool active = false;
    bool isBounceTransition = false;
    std::string fromModeId;
    std::string toModeId;
    int fromWidth = 0;
    int fromHeight = 0;
    int fromX = 0;
    int fromY = 0;
    int currentX = 0;
    int currentY = 0;
    int currentWidth = 0;
    int currentHeight = 0;
    int toX = 0;
    int toY = 0;
    int toWidth = 0;
    int toHeight = 0;
    int fromNativeWidth = 0;
    int fromNativeHeight = 0;
    int toNativeWidth = 0;
    int toNativeHeight = 0;
    GameTransitionType gameTransition = GameTransitionType::Cut;
    OverlayTransitionType overlayTransition = OverlayTransitionType::Cut;
    BackgroundTransitionType backgroundTransition = BackgroundTransitionType::Cut;
    float progress = 1.0f;
    float moveProgress = 1.0f;

    std::chrono::steady_clock::time_point startTime;
};
extern ViewportTransitionSnapshot g_viewportTransitionSnapshots[2];
extern std::atomic<int> g_viewportTransitionSnapshotIndex;

extern std::string g_lastFrameModeIdBuffers[2];
extern std::atomic<int> g_lastFrameModeIdIndex;

struct PendingModeSwitch {
    bool pending = false;
    std::string modeId;
    std::string source;

    bool isPreview = false;
    std::string previewFromModeId;

    bool forceInstant = false;
};
extern PendingModeSwitch g_pendingModeSwitch;
extern std::mutex g_pendingModeSwitchMutex;

// When GUI spinners change mode dimensions, the change is deferred to the game thread
// to avoid race conditions between render thread (GUI) and game thread (reading config)
struct PendingDimensionChange {
    bool pending = false;
    std::string modeId;
    int newWidth = 0;
    int newHeight = 0;
    bool sendWmSize = false;
};
extern PendingDimensionChange g_pendingDimensionChange;
extern std::mutex g_pendingDimensionChangeMutex;

extern std::atomic<double> g_lastFrameTimeMs;
extern std::atomic<double> g_originalFrameTimeMs;

extern std::atomic<bool> g_showPausedWarning;
extern std::chrono::steady_clock::time_point g_pausedWarningStartTime;
extern std::mutex g_pausedWarningMutex;

extern std::atomic<bool> g_imageDragMode;
extern std::string g_draggedImageName;
extern std::mutex g_imageDragMutex;

extern std::atomic<bool> g_windowOverlayDragMode;

extern std::string g_gameStateBuffers[2];
extern std::atomic<int> g_currentGameStateIndex;

void Log(const std::string& message);
void Log(const std::wstring& message);
std::wstring Utf8ToWide(const std::string& utf8_string);

void RenderSettingsGUI();
void RenderConfigErrorGUI();
void RenderPerformanceOverlay(bool showPerformanceOverlay);
void RenderProfilerOverlay(bool showProfiler, bool showPerformanceOverlay);

extern std::atomic<bool> g_welcomeToastVisible;
extern std::atomic<bool> g_configurePromptDismissedThisSession;
void RenderWelcomeToast(bool isFullscreen);

void HandleConfigLoadFailed(HDC hDc, BOOL (*oWglSwapBuffers)(HDC));
void RenderImGuiWithStateProtection(bool useFullProtection);
void SaveConfig();
void SaveConfigImmediate();
void ApplyAppearanceConfig();
void SaveTheme();
void LoadTheme();
void LoadConfig();
void CopyToClipboard(HWND hwnd, const std::string& text);

std::string GameTransitionTypeToString(GameTransitionType type);
GameTransitionType StringToGameTransitionType(const std::string& str);
std::string OverlayTransitionTypeToString(OverlayTransitionType type);
OverlayTransitionType StringToOverlayTransitionType(const std::string& str);
std::string BackgroundTransitionTypeToString(BackgroundTransitionType type);
BackgroundTransitionType StringToBackgroundTransitionType(const std::string& str);

void RebuildHotkeyMainKeys();
void RebuildHotkeyMainKeys_Internal(); // Internal version - requires locks already held

void StartModeTransition(const std::string& fromModeId, const std::string& toModeId, int fromWidth, int fromHeight, int fromX, int fromY,
                         int toWidth, int toHeight, int toX, int toY, const ModeConfig& toMode);
void UpdateModeTransition();
bool IsModeTransitionActive();
GameTransitionType GetGameTransitionType();
OverlayTransitionType GetOverlayTransitionType();
BackgroundTransitionType GetBackgroundTransitionType();
void GetAnimatedModeViewport(int& outWidth, int& outHeight);