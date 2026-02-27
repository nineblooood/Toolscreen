#include "logic_thread.h"
#include "expression_parser.h"
#include "gui.h"
#include "mirror_thread.h"
#include "profiler.h"
#include "render.h"
#include "utils.h"
#include "version.h"
#include <Windows.h>
#include <thread>

std::atomic<bool> g_logicThreadRunning{ false };
static std::thread g_logicThread;
static std::atomic<bool> g_logicThreadShouldStop{ false };

extern std::atomic<bool> g_graphicsHookDetected;
extern std::atomic<HMODULE> g_graphicsHookModule;
extern std::chrono::steady_clock::time_point g_lastGraphicsHookCheck;
extern const int GRAPHICS_HOOK_CHECK_INTERVAL_MS;

extern std::atomic<HWND> g_minecraftHwnd;
extern std::atomic<bool> g_configLoaded;
extern Config g_config;

extern std::string g_gameStateBuffers[2];
extern std::atomic<int> g_currentGameStateIndex;

extern std::atomic<bool> g_windowsMouseSpeedApplied;
extern int g_originalWindowsMouseSpeed;

extern std::atomic<bool> g_isShuttingDown;

extern PendingModeSwitch g_pendingModeSwitch;
extern std::mutex g_pendingModeSwitchMutex;

extern PendingDimensionChange g_pendingDimensionChange;
extern std::mutex g_pendingDimensionChangeMutex;

extern GameVersion g_gameVersion;

void ApplyWindowsMouseSpeed();

// Double-buffered viewport cache for lock-free access by hkglViewport
CachedModeViewport g_viewportModeCache[2];
std::atomic<int> g_viewportModeCacheIndex{ 0 };
static std::string s_lastCachedModeId;
static uint64_t s_lastCachedViewportSnapshotVersion = 0;

static bool s_wasInWorld = false;
static int s_lastAppliedWindowsMouseSpeed = -1;
static std::string s_previousGameStateForReset = "init";

static std::atomic<int> s_cachedScreenWidth{ 0 };
static std::atomic<int> s_cachedScreenHeight{ 0 };

// - Periodic refresh is a safety net in case move messages are missed.
// - If another thread detects a size change and updates the cache, it requests
//   an expression-dimension recalculation which MUST occur on the logic thread.
static std::atomic<bool> s_screenMetricsDirty{ true };
static std::atomic<bool> s_screenMetricsRecalcRequested{ false };
static std::atomic<ULONGLONG> s_lastScreenMetricsRefreshMs{ 0 };
static std::atomic<bool> s_startupMetricsResyncPending{ true };

static void ComputeScreenMetricsForGameWindow(int& outW, int& outH) {
    outW = 0;
    outH = 0;

    HWND hwnd = g_minecraftHwnd.load(std::memory_order_relaxed);
    if (hwnd && IsWindow(hwnd)) {
        RECT clientRect{};
        if (GetClientRect(hwnd, &clientRect)) {
            const int clientW = clientRect.right - clientRect.left;
            const int clientH = clientRect.bottom - clientRect.top;
            if (clientW > 0 && clientH > 0) {
                outW = clientW;
                outH = clientH;
                return;
            }
        }

        // Important: do NOT fall back to monitor metrics while a valid game window exists.
        // During startup/minimize/live-resize, client size may be transiently zero; using
        // monitor size in that window corrupts relative mode sizing calculations.
        return;
    }

    // Only when there is no valid game window yet, fall back to monitor/system size
    // so early startup code can still proceed.
    if (!GetMonitorSizeForWindow(hwnd, outW, outH)) {
        outW = GetSystemMetrics(SM_CXSCREEN);
        outH = GetSystemMetrics(SM_CYSCREEN);
    }
}

static bool RefreshCachedScreenMetricsIfNeeded(bool requestRecalcOnChange) {
    constexpr ULONGLONG kPeriodicRefreshMs = 250; // fast enough to catch monitor moves, cheap enough for render thread callers
    ULONGLONG now = GetTickCount64();

    bool forced = s_screenMetricsDirty.exchange(false, std::memory_order_relaxed);
    ULONGLONG last = s_lastScreenMetricsRefreshMs.load(std::memory_order_relaxed);
    bool periodic = (now - last) >= kPeriodicRefreshMs;

    if (!forced && !periodic) { return false; }
    s_lastScreenMetricsRefreshMs.store(now, std::memory_order_relaxed);

    int newW = 0, newH = 0;
    ComputeScreenMetricsForGameWindow(newW, newH);
    if (newW <= 0 || newH <= 0) {
        // If a game window exists but currently reports an invalid/zero client area
        // (common during startup/minimize/live-resize), invalidate cached dimensions.
        // This prevents stale monitor-sized values from being reused for relative-mode math.
        HWND hwnd = g_minecraftHwnd.load(std::memory_order_relaxed);
        if (hwnd && IsWindow(hwnd)) {
            int prevW = s_cachedScreenWidth.load(std::memory_order_relaxed);
            int prevH = s_cachedScreenHeight.load(std::memory_order_relaxed);
            if (prevW != 0 || prevH != 0) {
                s_cachedScreenWidth.store(0, std::memory_order_relaxed);
                s_cachedScreenHeight.store(0, std::memory_order_relaxed);
            }
        }
        return false;
    }

    int prevW = s_cachedScreenWidth.load(std::memory_order_relaxed);
    int prevH = s_cachedScreenHeight.load(std::memory_order_relaxed);

    if (prevW != newW || prevH != newH) {
        s_cachedScreenWidth.store(newW, std::memory_order_relaxed);
        s_cachedScreenHeight.store(newH, std::memory_order_relaxed);

        if (requestRecalcOnChange) { s_screenMetricsRecalcRequested.store(true, std::memory_order_relaxed); }
        return true;
    }

    return false;
}

void InvalidateCachedScreenMetrics() {
    s_screenMetricsDirty.store(true, std::memory_order_relaxed);
}

void RequestScreenMetricsRecalculation() {
    s_screenMetricsDirty.store(true, std::memory_order_relaxed);
    s_screenMetricsRecalcRequested.store(true, std::memory_order_relaxed);
}

static std::vector<std::string> s_lastActiveMirrorIds;
static std::string s_lastMirrorConfigModeId;
static uint64_t s_lastMirrorConfigSnapshotVersion = 0;

void UpdateActiveMirrorConfigs() {
    PROFILE_SCOPE_CAT("LT Mirror Configs", "Logic Thread");

    // Use config snapshot for thread-safe access to modes/mirrors/mirrorGroups
    auto cfgSnap = GetConfigSnapshot();
    if (!cfgSnap) return;
    const Config& cfg = *cfgSnap;

    const uint64_t snapVer = g_configSnapshotVersion.load(std::memory_order_acquire);

    // Get current mode ID from double-buffer (lock-free)
    std::string currentModeId = g_modeIdBuffers[g_currentModeIdIndex.load(std::memory_order_acquire)];

    if (currentModeId == s_lastMirrorConfigModeId && snapVer == s_lastMirrorConfigSnapshotVersion) {
        return;
    }
    const ModeConfig* mode = GetModeFromSnapshot(cfg, currentModeId);
    if (!mode) { return; }

    std::vector<std::string> currentMirrorIds = mode->mirrorIds;
    for (const auto& groupName : mode->mirrorGroupIds) {
        for (const auto& group : cfg.mirrorGroups) {
            if (group.name == groupName) {
                for (const auto& item : group.mirrors) {
                    if (std::find(currentMirrorIds.begin(), currentMirrorIds.end(), item.mirrorId) == currentMirrorIds.end()) {
                        currentMirrorIds.push_back(item.mirrorId);
                    }
                }
                break;
            }
        }
    }

    if (currentMirrorIds != s_lastActiveMirrorIds) {
        std::vector<MirrorConfig> activeMirrorsForCapture;
        activeMirrorsForCapture.reserve(currentMirrorIds.size());
        for (const auto& mirrorId : currentMirrorIds) {
            for (const auto& mirror : cfg.mirrors) {
                if (mirror.name == mirrorId) {
                    MirrorConfig activeMirror = mirror;

                    for (const auto& groupName : mode->mirrorGroupIds) {
                        for (const auto& group : cfg.mirrorGroups) {
                            if (group.name == groupName) {
                                for (const auto& item : group.mirrors) {
                                    if (!item.enabled) continue;
                                    if (item.mirrorId == mirrorId) {
                                        int groupX = group.output.x;
                                        int groupY = group.output.y;
                                        if (group.output.useRelativePosition) {
                                            int screenW = GetCachedScreenWidth();
                                            int screenH = GetCachedScreenHeight();
                                            groupX = static_cast<int>(group.output.relativeX * screenW);
                                            groupY = static_cast<int>(group.output.relativeY * screenH);
                                        }
                                        activeMirror.output.x = groupX + item.offsetX;
                                        activeMirror.output.y = groupY + item.offsetY;
                                        activeMirror.output.relativeTo = group.output.relativeTo;
                                        activeMirror.output.useRelativePosition = group.output.useRelativePosition;
                                        activeMirror.output.relativeX = group.output.relativeX;
                                        activeMirror.output.relativeY = group.output.relativeY;
                                        if (item.widthPercent != 1.0f || item.heightPercent != 1.0f) {
                                            activeMirror.output.separateScale = true;
                                            float baseScaleX = mirror.output.separateScale ? mirror.output.scaleX : mirror.output.scale;
                                            float baseScaleY = mirror.output.separateScale ? mirror.output.scaleY : mirror.output.scale;
                                            activeMirror.output.scaleX = baseScaleX * item.widthPercent;
                                            activeMirror.output.scaleY = baseScaleY * item.heightPercent;
                                        }
                                        break;
                                    }
                                }
                                break;
                            }
                        }
                    }

                    activeMirrorsForCapture.push_back(activeMirror);
                    break;
                }
            }
        }
        UpdateMirrorCaptureConfigs(activeMirrorsForCapture);
        s_lastActiveMirrorIds = currentMirrorIds;
    }

    s_lastMirrorConfigModeId = currentModeId;
    s_lastMirrorConfigSnapshotVersion = snapVer;
}

void UpdateCachedScreenMetrics() {
    PROFILE_SCOPE_CAT("LT Screen Metrics", "Logic Thread");

    const bool startupPending = s_startupMetricsResyncPending.load(std::memory_order_relaxed);
    bool startupClientReady = false;
    if (startupPending) {
        HWND hwnd = g_minecraftHwnd.load(std::memory_order_relaxed);
        if (hwnd) {
            RECT clientRect{};
            if (GetClientRect(hwnd, &clientRect)) {
                startupClientReady = (clientRect.right - clientRect.left) > 0 && (clientRect.bottom - clientRect.top) > 0;
            }
        }

        // Keep requesting refresh/recalc until the client area is valid at least once.
        s_screenMetricsDirty.store(true, std::memory_order_relaxed);
        if (!startupClientReady) {
            s_screenMetricsRecalcRequested.store(true, std::memory_order_relaxed);
        }
    }

    // Note: other threads may refresh the cache (to avoid returning stale values),
    int prevWidth = s_cachedScreenWidth.load(std::memory_order_relaxed);
    int prevHeight = s_cachedScreenHeight.load(std::memory_order_relaxed);

    bool changed = RefreshCachedScreenMetricsIfNeeded(/*requestRecalcOnChange=*/false);
    bool recalcRequested = s_screenMetricsRecalcRequested.exchange(false, std::memory_order_relaxed);

    int newWidth = s_cachedScreenWidth.load(std::memory_order_relaxed);
    int newHeight = s_cachedScreenHeight.load(std::memory_order_relaxed);

    // Recalculate expression/relative-based dimensions if screen size changed
    // or if another thread explicitly requested it.
    // Require only that current metrics are valid so resize after startup/minimize still triggers recalculation.
    const bool startupShouldRunNow = startupPending && startupClientReady;
    if (newWidth > 0 && newHeight > 0 && (startupShouldRunNow || changed || recalcRequested || prevWidth != newWidth || prevHeight != newHeight)) {
        std::string currentModeId = g_modeIdBuffers[g_currentModeIdIndex.load(std::memory_order_acquire)];
        int beforeModeW = 0;
        int beforeModeH = 0;
        if (ModeConfig* currentModeBefore = GetModeMutable(currentModeId)) {
            beforeModeW = currentModeBefore->width;
            beforeModeH = currentModeBefore->height;
        }

        RecalculateExpressionDimensions();

        int afterModeW = 0;
        int afterModeH = 0;
        if (ModeConfig* currentModeAfter = GetModeMutable(currentModeId)) {
            afterModeW = currentModeAfter->width;
            afterModeH = currentModeAfter->height;
        }

        const bool modeSizeChanged = (afterModeW != beforeModeW || afterModeH != beforeModeH);
        bool clientSizeDiffersFromMode = false;
        {
            HWND hwnd = g_minecraftHwnd.load(std::memory_order_relaxed);
            if (hwnd && afterModeW > 0 && afterModeH > 0) {
                RECT clientRect{};
                if (GetClientRect(hwnd, &clientRect)) {
                    const int clientW = clientRect.right - clientRect.left;
                    const int clientH = clientRect.bottom - clientRect.top;
                    if (clientW > 0 && clientH > 0) {
                        clientSizeDiffersFromMode = (clientW != afterModeW) || (clientH != afterModeH);
                    }
                }
            }
        }

        const bool windowSizeChanged = changed || (prevWidth != newWidth) || (prevHeight != newHeight);
        const bool shouldEnforceForExternalResize = IsFullscreen() && windowSizeChanged && clientSizeDiffersFromMode;
        const bool shouldEnforceModeSize = startupShouldRunNow || modeSizeChanged || shouldEnforceForExternalResize;

        if (afterModeW > 0 && afterModeH > 0 && shouldEnforceModeSize && IsResolutionChangeSupported(g_gameVersion)) {
            HWND hwnd = g_minecraftHwnd.load(std::memory_order_relaxed);
            if (hwnd) { PostMessage(hwnd, WM_SIZE, SIZE_RESTORED, MAKELPARAM(afterModeW, afterModeH)); }
        }

        // Publish updated snapshot so reader threads see the recalculated dimensions.
        PublishConfigSnapshot();

        if (startupShouldRunNow) { s_startupMetricsResyncPending.store(false, std::memory_order_relaxed); }
    }
}

int GetCachedScreenWidth() {
    RefreshCachedScreenMetricsIfNeeded(/*requestRecalcOnChange=*/true);

    int w = s_cachedScreenWidth.load(std::memory_order_relaxed);
    if (w == 0) {
        // Startup fallback if logic thread hasn't populated the cache yet.
        int tmpW = 0, tmpH = 0;
        ComputeScreenMetricsForGameWindow(tmpW, tmpH);
        if (tmpW > 0) {
            s_cachedScreenWidth.store(tmpW, std::memory_order_relaxed);
            s_cachedScreenHeight.store(tmpH, std::memory_order_relaxed);
            w = tmpW;
        }
    }
    return w;
}

int GetCachedScreenHeight() {
    RefreshCachedScreenMetricsIfNeeded(/*requestRecalcOnChange=*/true);

    int h = s_cachedScreenHeight.load(std::memory_order_relaxed);
    if (h == 0) {
        // Startup fallback if logic thread hasn't populated the cache yet.
        int tmpW = 0, tmpH = 0;
        ComputeScreenMetricsForGameWindow(tmpW, tmpH);
        if (tmpH > 0) {
            s_cachedScreenWidth.store(tmpW, std::memory_order_relaxed);
            s_cachedScreenHeight.store(tmpH, std::memory_order_relaxed);
            h = tmpH;
        }
    }
    return h;
}

void UpdateCachedViewportMode() {
    PROFILE_SCOPE_CAT("LT Viewport Cache", "Logic Thread");

    // Read current mode ID from double-buffer (lock-free)
    std::string currentModeId = g_modeIdBuffers[g_currentModeIdIndex.load(std::memory_order_acquire)];
    const uint64_t snapVer = g_configSnapshotVersion.load(std::memory_order_acquire);

    // Also force periodic refresh every 60 ticks (~1 second) as a safety net
    static int s_ticksSinceRefresh = 0;
    bool guiOpen = g_showGui.load(std::memory_order_relaxed);
    bool periodicRefresh = (++s_ticksSinceRefresh >= 60);

    if (currentModeId == s_lastCachedModeId && snapVer == s_lastCachedViewportSnapshotVersion && !guiOpen && !periodicRefresh) {
        return;
    }

    if (periodicRefresh) { s_ticksSinceRefresh = 0; }

    // Get mode data via config snapshot (thread-safe, lock-free)
    auto cfgSnap = GetConfigSnapshot();
    if (!cfgSnap) return;
    const ModeConfig* mode = GetModeFromSnapshot(*cfgSnap, currentModeId);

    int nextIndex = 1 - g_viewportModeCacheIndex.load(std::memory_order_relaxed);
    CachedModeViewport& cache = g_viewportModeCache[nextIndex];

    if (mode) {
        cache.width = mode->width;
        cache.height = mode->height;
        cache.stretchEnabled = mode->stretch.enabled;
        cache.stretchX = mode->stretch.x;
        cache.stretchY = mode->stretch.y;
        cache.stretchWidth = mode->stretch.width;
        cache.stretchHeight = mode->stretch.height;
        cache.valid = true;
    } else {
        cache.valid = false;
    }

    // Atomic swap to make new cache visible
    g_viewportModeCacheIndex.store(nextIndex, std::memory_order_release);
    s_lastCachedModeId = currentModeId;
    s_lastCachedViewportSnapshotVersion = snapVer;
}

void PollObsGraphicsHook() {
    PROFILE_SCOPE_CAT("LT OBS Hook Poll", "Logic Thread");
    auto now = std::chrono::steady_clock::now();
    auto msSinceLastCheck = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_lastGraphicsHookCheck).count();

    if (msSinceLastCheck >= GRAPHICS_HOOK_CHECK_INTERVAL_MS) {
        g_lastGraphicsHookCheck = now;
        HMODULE hookModule = GetModuleHandleA("graphics-hook64.dll");
        bool wasDetected = g_graphicsHookDetected.load();
        bool nowDetected = (hookModule != NULL);

        if (nowDetected != wasDetected) {
            g_graphicsHookDetected.store(nowDetected);
            g_graphicsHookModule.store(hookModule);
            if (nowDetected) {
                Log("[OBS] graphics-hook64.dll DETECTED - OBS overlay active");
            } else {
                Log("[OBS] graphics-hook64.dll UNLOADED - OBS overlay inactive");
            }
        }
    }
}

void CheckWorldExitReset() {
    PROFILE_SCOPE_CAT("LT World Exit Check", "Logic Thread");

    // Get current game state from lock-free buffer
    std::string currentGameState = g_gameStateBuffers[g_currentGameStateIndex.load(std::memory_order_acquire)];
    bool isInWorld = (currentGameState.find("inworld") != std::string::npos);

    if (s_wasInWorld && !isInWorld) {
        auto cfgSnap = GetConfigSnapshot();
        if (cfgSnap) {
            const Config& cfg = *cfgSnap;
            for (size_t i = 0; i < cfg.hotkeys.size(); ++i) {
                const auto& hotkey = cfg.hotkeys[i];
                if (!hotkey.secondaryMode.empty() && GetHotkeySecondaryMode(i) != hotkey.secondaryMode) {
                    SetHotkeySecondaryMode(i, hotkey.secondaryMode);
                    Log("[Hotkey] Reset secondary mode for hotkey to: " + hotkey.secondaryMode);
                }
            }
        }
    }
    s_wasInWorld = isInWorld;
}

void CheckWindowsMouseSpeedChange() {
    PROFILE_SCOPE_CAT("LT Mouse Speed Check", "Logic Thread");
    auto cfgSnap = GetConfigSnapshot();
    int currentWindowsMouseSpeed = cfgSnap ? cfgSnap->windowsMouseSpeed : 0;
    if (currentWindowsMouseSpeed != s_lastAppliedWindowsMouseSpeed) {
        ApplyWindowsMouseSpeed();
        s_lastAppliedWindowsMouseSpeed = currentWindowsMouseSpeed;
    }
}

void ProcessPendingModeSwitch() {
    PROFILE_SCOPE_CAT("LT Mode Switch", "Logic Thread");
    std::lock_guard<std::mutex> pendingLock(g_pendingModeSwitchMutex);
    if (!g_pendingModeSwitch.pending) { return; }

    if (g_pendingModeSwitch.isPreview && !g_pendingModeSwitch.previewFromModeId.empty()) {
        Log("[GUI] Processing preview mode switch: " + g_pendingModeSwitch.previewFromModeId + " -> " + g_pendingModeSwitch.modeId);

        std::string fromModeId = g_pendingModeSwitch.previewFromModeId;
        std::string toModeId = g_pendingModeSwitch.modeId;

        SwitchToMode(fromModeId, "Preview (instant)", /*forceCut=*/true);

        SwitchToMode(toModeId, "Preview (animated)");
    } else {
        LogCategory("gui", "[GUI] Processing deferred mode switch to: " + g_pendingModeSwitch.modeId +
                               " (source: " + g_pendingModeSwitch.source + ")");

        // This avoids cross-thread mutation of g_config from the logic thread
        SwitchToMode(g_pendingModeSwitch.modeId, g_pendingModeSwitch.source,
                     /*forceCut=*/g_pendingModeSwitch.forceInstant);
    }

    g_pendingModeSwitch.pending = false;
    g_pendingModeSwitch.isPreview = false;
    g_pendingModeSwitch.forceInstant = false;
    g_pendingModeSwitch.modeId.clear();
    g_pendingModeSwitch.source.clear();
    g_pendingModeSwitch.previewFromModeId.clear();
}

// This processes dimension changes from the GUI (render thread) on the logic thread
// to avoid race conditions between render thread modifying config and game thread reading it
void ProcessPendingDimensionChange() {
    PROFILE_SCOPE_CAT("LT Dimension Change", "Logic Thread");
    std::lock_guard<std::mutex> lock(g_pendingDimensionChangeMutex);
    if (!g_pendingDimensionChange.pending) { return; }

    ModeConfig* mode = GetModeMutable(g_pendingDimensionChange.modeId);
    if (mode) {
        if (g_pendingDimensionChange.newWidth > 0) {
            mode->width = EqualsIgnoreCase(mode->id, "Thin") ? (std::max)(330, g_pendingDimensionChange.newWidth)
                                                               : g_pendingDimensionChange.newWidth;
            mode->widthExpr.clear();
            mode->relativeWidth = -1.0f;
        }
        if (g_pendingDimensionChange.newHeight > 0) {
            mode->height = g_pendingDimensionChange.newHeight;
            mode->heightExpr.clear();
            mode->relativeHeight = -1.0f;
        }

        const bool hasRelativeWidth = (mode->relativeWidth >= 0.0f && mode->relativeWidth <= 1.0f);
        const bool hasRelativeHeight = (mode->relativeHeight >= 0.0f && mode->relativeHeight <= 1.0f);
        if (!hasRelativeWidth && !hasRelativeHeight) { mode->useRelativeSize = false; }

        ModeConfig* eyezoomMode = GetModeMutable("EyeZoom");
        ModeConfig* preemptiveMode = GetModeMutable("Preemptive");
        bool preemptiveWasResynced = false;
        if (eyezoomMode && preemptiveMode) {
            if (!preemptiveMode->widthExpr.empty() || !preemptiveMode->heightExpr.empty() || preemptiveMode->useRelativeSize ||
                preemptiveMode->relativeWidth >= 0.0f || preemptiveMode->relativeHeight >= 0.0f) {
                preemptiveMode->widthExpr.clear();
                preemptiveMode->heightExpr.clear();
                preemptiveMode->useRelativeSize = false;
                preemptiveMode->relativeWidth = -1.0f;
                preemptiveMode->relativeHeight = -1.0f;
                preemptiveWasResynced = true;
            }

            if (preemptiveMode->width != eyezoomMode->width) {
                preemptiveMode->width = eyezoomMode->width;
                preemptiveWasResynced = true;
            }
            if (preemptiveMode->height != eyezoomMode->height) {
                preemptiveMode->height = eyezoomMode->height;
                preemptiveWasResynced = true;
            }
        }

        if (g_pendingDimensionChange.sendWmSize && g_currentModeId == g_pendingDimensionChange.modeId) {
            HWND hwnd = g_minecraftHwnd.load();
            if (hwnd) { PostMessage(hwnd, WM_SIZE, SIZE_RESTORED, MAKELPARAM(mode->width, mode->height)); }
        }

        if (g_pendingDimensionChange.sendWmSize && g_currentModeId == "Preemptive" && g_pendingDimensionChange.modeId == "EyeZoom" &&
            preemptiveMode) {
            HWND hwnd = g_minecraftHwnd.load();
            if (hwnd) { PostMessage(hwnd, WM_SIZE, SIZE_RESTORED, MAKELPARAM(preemptiveMode->width, preemptiveMode->height)); }
        }

        if (preemptiveWasResynced) { g_configIsDirty = true; }

        g_configIsDirty = true;
    }

    g_pendingDimensionChange.pending = false;
    g_pendingDimensionChange.modeId.clear();
    g_pendingDimensionChange.newWidth = 0;
    g_pendingDimensionChange.newHeight = 0;
    g_pendingDimensionChange.sendWmSize = false;
}

void CheckGameStateReset() {
    PROFILE_SCOPE_CAT("LT Game State Reset", "Logic Thread");

    if (!IsResolutionChangeSupported(g_gameVersion)) { return; }

    // Get current game state from lock-free buffer
    std::string localGameState = g_gameStateBuffers[g_currentGameStateIndex.load(std::memory_order_acquire)];

    if (isWallTitleOrWaiting(localGameState) && !isWallTitleOrWaiting(s_previousGameStateForReset)) {
        auto cfgSnap = GetConfigSnapshot();
        if (cfgSnap) {
            const Config& cfg = *cfgSnap;
            for (size_t i = 0; i < cfg.hotkeys.size(); ++i) {
                if (GetHotkeySecondaryMode(i) != cfg.hotkeys[i].secondaryMode) { SetHotkeySecondaryMode(i, cfg.hotkeys[i].secondaryMode); }
            }

            std::string targetMode = cfg.defaultMode;
            Log("[LogicThread] Reset all hotkey secondary modes to default due to wall/title/waiting state.");
            SwitchToMode(targetMode, "game state reset", /*forceCut=*/true);
        }
    }

    s_previousGameStateForReset = localGameState;
}

static void CheckAutoBorderless() {
    static bool s_checked = false;
    if (s_checked) { return; }

    HWND hwnd = g_minecraftHwnd.load(std::memory_order_relaxed);
    if (!hwnd) { return; }

    s_checked = true;

    if (!g_config.autoBorderless) { return; }

    ToggleBorderlessWindowedFullscreen(hwnd);
    Log("[LogicThread] Auto-borderless applied");
}

static void LogicThreadFunc() {
    LogCategory("init", "[LogicThread] Started");

    const auto tickInterval = std::chrono::milliseconds(16);

    while (!g_logicThreadShouldStop.load()) {
        PROFILE_SCOPE_CAT("Logic Thread Tick", "Logic Thread");
        auto tickStart = std::chrono::steady_clock::now();

        if (g_isShuttingDown.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        if (!g_configLoaded.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        UpdateCachedScreenMetrics();
        UpdateCachedViewportMode();
        UpdateActiveMirrorConfigs();
        PollObsGraphicsHook();
        CheckWorldExitReset();
        CheckWindowsMouseSpeedChange();
        ProcessPendingModeSwitch();
        ProcessPendingDimensionChange();
        CheckGameStateReset();
        CheckAutoBorderless();

        auto tickEnd = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(tickEnd - tickStart);
        if (elapsed < tickInterval) { std::this_thread::sleep_for(tickInterval - elapsed); }
    }

    Log("[LogicThread] Stopped");
}

void StartLogicThread() {
    if (g_logicThreadRunning.load()) {
        Log("[LogicThread] Already running, not starting again");
        return;
    }

    Log("[LogicThread] Starting logic thread...");
    g_logicThreadShouldStop.store(false);
    s_startupMetricsResyncPending.store(true, std::memory_order_relaxed);
    s_screenMetricsDirty.store(true, std::memory_order_relaxed);
    s_screenMetricsRecalcRequested.store(true, std::memory_order_relaxed);

    g_logicThread = std::thread(LogicThreadFunc);
    g_logicThreadRunning.store(true);

    LogCategory("init", "[LogicThread] Logic thread started");
}

void StopLogicThread() {
    if (!g_logicThreadRunning.load()) { return; }

    Log("[LogicThread] Stopping logic thread...");
    g_logicThreadShouldStop.store(true);

    if (g_logicThread.joinable()) { g_logicThread.join(); }

    g_logicThreadRunning.store(false);
    Log("[LogicThread] Logic thread stopped");
}


