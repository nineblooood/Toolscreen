#include "fake_cursor.h"
#include "gui.h"
#include "imgui_cache.h"
#include "input_hook.h"
#include "logic_thread.h"
#include "mirror_thread.h"
#include "obs_thread.h"
#include "profiler.h"
#include "render.h"
#include "render_thread.h"
#include "resource.h"
#include "shared_contexts.h"
#include "hook_chain.h"
#include "utils.h"
#include "version.h"
#include "virtual_camera.h"
#include "window_overlay.h"

#include "MinHook.h"
#include <array>
#include <DbgHelp.h>
#include <Psapi.h>
#include <ShlObj.h>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <intrin.h>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <shared_mutex>
#include <sstream>
#include <synchapi.h>
#include <thread>
#include <windowsx.h>

#pragma comment(lib, "Comdlg32.lib")
#pragma comment(lib, "Msimg32.lib")
#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "libglew32.lib")
#pragma comment(lib, "DbgHelp.lib")

#define STB_IMAGE_IMPLEMENTATION
#include "imgui_impl_opengl3.h"
#include "imgui_impl_win32.h"
#include "stb_image.h"

Config g_config;
std::atomic<bool> g_configIsDirty{ false };

std::atomic<uint64_t> g_configSnapshotVersion{ 0 };

// CONFIG SNAPSHOT (RCU) - Lock-free immutable config for reader threads
// The mutable g_config is only touched by the GUI/main thread.
// Reader threads call GetConfigSnapshot() for a safe, lock-free snapshot.
static std::shared_ptr<const Config> g_configSnapshot;

void PublishConfigSnapshot() {
    auto snapshot = std::make_shared<const Config>(g_config);
    // Lock-free publish: atomic store of shared_ptr.
    std::atomic_store_explicit(&g_configSnapshot, std::move(snapshot), std::memory_order_release);

    g_configSnapshotVersion.fetch_add(1, std::memory_order_release);
}

std::shared_ptr<const Config> GetConfigSnapshot() {
    // Lock-free read: atomic load of shared_ptr.
    return std::atomic_load_explicit(&g_configSnapshot, std::memory_order_acquire);
}

// HOTKEY SECONDARY MODE STATE - Thread-safe runtime state separated from Config
static std::vector<std::string> g_hotkeySecondaryModes;
std::mutex g_hotkeySecondaryModesMutex;

std::string GetHotkeySecondaryMode(size_t hotkeyIndex) {
    std::lock_guard<std::mutex> lock(g_hotkeySecondaryModesMutex);
    if (hotkeyIndex < g_hotkeySecondaryModes.size()) { return g_hotkeySecondaryModes[hotkeyIndex]; }
    return "";
}

void SetHotkeySecondaryMode(size_t hotkeyIndex, const std::string& mode) {
    std::lock_guard<std::mutex> lock(g_hotkeySecondaryModesMutex);
    if (hotkeyIndex >= g_hotkeySecondaryModes.size()) { g_hotkeySecondaryModes.resize(hotkeyIndex + 1); }
    g_hotkeySecondaryModes[hotkeyIndex] = mode;
}

void ResetAllHotkeySecondaryModes() {
    std::lock_guard<std::mutex> lock(g_hotkeySecondaryModesMutex);
    g_hotkeySecondaryModes.resize(g_config.hotkeys.size());
    for (size_t i = 0; i < g_config.hotkeys.size(); ++i) { g_hotkeySecondaryModes[i] = g_config.hotkeys[i].secondaryMode; }
}

void ResetAllHotkeySecondaryModes(const Config& config) {
    std::lock_guard<std::mutex> lock(g_hotkeySecondaryModesMutex);
    g_hotkeySecondaryModes.resize(config.hotkeys.size());
    for (size_t i = 0; i < config.hotkeys.size(); ++i) { g_hotkeySecondaryModes[i] = config.hotkeys[i].secondaryMode; }
}

void ResizeHotkeySecondaryModes(size_t count) {
    std::lock_guard<std::mutex> lock(g_hotkeySecondaryModesMutex);
    g_hotkeySecondaryModes.resize(count);
}

TempSensitivityOverride g_tempSensitivityOverride;
std::mutex g_tempSensitivityMutex;

void ClearTempSensitivityOverride() {
    std::lock_guard<std::mutex> lock(g_tempSensitivityMutex);
    g_tempSensitivityOverride.active = false;
    g_tempSensitivityOverride.sensitivityX = 1.0f;
    g_tempSensitivityOverride.sensitivityY = 1.0f;
    g_tempSensitivityOverride.activeSensHotkeyIndex = -1;
}

std::atomic<bool> g_cursorsNeedReload{ false };
std::atomic<bool> g_showGui{ false };
std::atomic<bool> g_imageOverlaysVisible{ true };
std::atomic<bool> g_windowOverlaysVisible{ true };
std::string g_currentlyEditingMirror;
std::atomic<HWND> g_minecraftHwnd{ NULL };
std::wstring g_toolscreenPath;
std::string g_currentModeId = "";
std::mutex g_modeIdMutex;
// Lock-free mode ID access (double-buffered) - input handlers read from these without locking
std::string g_modeIdBuffers[2] = { "", "" };
std::atomic<int> g_currentModeIdIndex{ 0 };
std::atomic<bool> g_screenshotRequested{ false };
std::atomic<bool> g_pendingImageLoad{ false };
std::string g_configLoadError;
std::mutex g_configErrorMutex;
std::wstring g_modeFilePath;
std::atomic<bool> g_configLoadFailed{ false };
std::atomic<bool> g_configLoaded{ false };
std::unordered_map<std::string, std::chrono::steady_clock::time_point> g_hotkeyTimestamps;
std::atomic<bool> g_guiNeedsRecenter{ true };
std::atomic<bool> g_wasCursorVisible{ true };
// Lock-free GUI toggle debounce timestamp
std::atomic<int64_t> g_lastGuiToggleTimeMs{ 0 };

enum CapturingState { NONE = 0, DISABLED = 1, NORMAL = 2 };
std::atomic<CapturingState> g_capturingMousePos{ CapturingState::NONE };
std::atomic<std::pair<int, int>> g_nextMouseXY{ std::make_pair(-1, -1) };

std::unordered_set<DWORD> g_hotkeyMainKeys;
std::mutex g_hotkeyMainKeysMutex;

std::mutex g_hotkeyTimestampsMutex;

// Track trigger-on-release hotkeys that are currently pressed
std::set<std::string> g_triggerOnReleasePending;
// Track which pending trigger-on-release hotkeys have been invalidated
std::set<std::string> g_triggerOnReleaseInvalidated;
std::mutex g_triggerOnReleaseMutex;

std::atomic<bool> g_imageDragMode{ false };
std::string g_draggedImageName = "";
std::mutex g_imageDragMutex;

std::atomic<bool> g_windowOverlayDragMode{ false };

std::ofstream logFile;
std::mutex g_logFileMutex;
HMODULE g_hModule = NULL;

GameVersion g_gameVersion;

bool g_glewLoaded = false;
WNDPROC g_originalWndProc = NULL;
std::atomic<HWND> g_subclassedHwnd{ NULL };
std::atomic<bool> g_hwndChanged{ false };
std::atomic<bool> g_isShuttingDown{ false };
std::atomic<bool> g_allImagesLoaded{ false };
std::atomic<bool> g_isTransitioningMode{ false };
std::atomic<bool> g_skipViewportAnimation{ false };
std::atomic<int> g_wmMouseMoveCount{ 0 };

static std::atomic<HGLRC> g_lastSeenGameGLContext{ NULL };

ModeTransitionAnimation g_modeTransition;
std::mutex g_modeTransitionMutex;
// Lock-free snapshot for viewport hook
ViewportTransitionSnapshot g_viewportTransitionSnapshots[2];
std::atomic<int> g_viewportTransitionSnapshotIndex{ 0 };

PendingModeSwitch g_pendingModeSwitch;
std::mutex g_pendingModeSwitchMutex;

PendingDimensionChange g_pendingDimensionChange;
std::mutex g_pendingDimensionChangeMutex;

std::atomic<double> g_lastFrameTimeMs{ 0.0 };
std::atomic<double> g_originalFrameTimeMs{ 0.0 };

std::chrono::high_resolution_clock::time_point g_lastFrameEndTime = std::chrono::high_resolution_clock::now();
std::mutex g_fpsLimitMutex;
HANDLE g_highResTimer = NULL;
int g_originalWindowsMouseSpeed = 0;                      // Original Windows mouse speed to restore on exit
std::atomic<bool> g_windowsMouseSpeedApplied{ false };
FILTERKEYS g_originalFilterKeys = { sizeof(FILTERKEYS) };
std::atomic<bool> g_filterKeysApplied{ false };
std::atomic<bool> g_originalFilterKeysCaptured{ false };

std::string g_lastFrameModeId = "";
std::mutex g_lastFrameModeIdMutex;
// Lock-free last frame mode ID for viewport hook
std::string g_lastFrameModeIdBuffers[2] = { "", "" };
std::atomic<int> g_lastFrameModeIdIndex{ 0 };
std::string g_gameStateBuffers[2] = { "title", "title" };
std::atomic<int> g_currentGameStateIndex{ 0 };
const ModeConfig* g_currentMode = nullptr;

std::atomic<bool> g_gameWindowActive{ false };

std::thread g_monitorThread;
std::thread g_imageMonitorThread;
static std::thread g_hookCompatThread;
static std::atomic<bool> g_stopHookCompat{ false };
HANDLE g_resizeThread = NULL;
std::atomic<bool> g_stopMonitoring{ false };
std::atomic<bool> g_stopImageMonitoring{ false };
std::wstring g_stateFilePath;
std::atomic<bool> g_isStateOutputAvailable{ false };

std::vector<DecodedImageData> g_decodedImagesQueue;
std::mutex g_decodedImagesMutex;

std::atomic<GLuint> g_cachedGameTextureId{ UINT_MAX };

std::atomic<HCURSOR> g_specialCursorHandle{ NULL };

std::atomic<bool> g_graphicsHookDetected{ false };
std::atomic<HMODULE> g_graphicsHookModule{ NULL };
std::chrono::steady_clock::time_point g_lastGraphicsHookCheck = std::chrono::steady_clock::now();
extern const int GRAPHICS_HOOK_CHECK_INTERVAL_MS = 2000;

std::atomic<bool> g_obsCaptureReady{ false };

void LoadConfig();
void SaveConfig();
void RenderSettingsGUI();
void AttemptAggressiveGlViewportHook();
GLuint CalculateGameTextureId();


bool SubclassGameWindow(HWND hwnd) {
    if (!hwnd) return false;

    if (g_isShuttingDown.load()) return false;

    HWND currentSubclassed = g_subclassedHwnd.load();
    if (currentSubclassed == hwnd && g_originalWndProc != NULL) {
        return true;
    }

    if (currentSubclassed != NULL && currentSubclassed != hwnd) {
        Log("Window handle changed from " + std::to_string(reinterpret_cast<uintptr_t>(currentSubclassed)) + " to " +
            std::to_string(reinterpret_cast<uintptr_t>(hwnd)) + " (likely fullscreen toggle)");
        g_originalWndProc = NULL;

        g_minecraftHwnd.store(hwnd);
        g_cachedGameTextureId.store(UINT_MAX);
        g_hwndChanged.store(true);
    }

    WNDPROC oldProc = (WNDPROC)SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR)SubclassedWndProc);
    if (oldProc) {
        g_originalWndProc = oldProc;
        g_subclassedHwnd.store(hwnd);
        Log("Successfully subclassed window: " + std::to_string(reinterpret_cast<uintptr_t>(hwnd)));
        return true;
    } else {
        Log("ERROR: Failed to subclass window: " + std::to_string(reinterpret_cast<uintptr_t>(hwnd)));
        return false;
    }
}

template <typename T> bool CreateHookOrDie(LPVOID pTarget, LPVOID pDetour, T** ppOriginal, const char* hookName) {
    if (pTarget == NULL) {
        std::string warnMsg = std::string("WARNING: ") + hookName + " function not found (NULL pointer)";
        Log(warnMsg);
        return false;
    }
    if (MH_CreateHook(pTarget, pDetour, reinterpret_cast<void**>(ppOriginal)) != MH_OK) {
        std::string errorMsg = std::string("ERROR: ") + hookName + " hook failed!";
        Log(errorMsg);
        return false;
    }
    LogCategory("init", "Created hook for " + std::string(hookName));
    return true;
}

// REQUIRES: g_configMutex and g_hotkeyMainKeysMutex must already be held by caller
void RebuildHotkeyMainKeys_Internal() {
    g_hotkeyMainKeys.clear();

    auto isModifier = [](DWORD key) {
        return key == VK_CONTROL || key == VK_LCONTROL || key == VK_RCONTROL || key == VK_SHIFT || key == VK_LSHIFT || key == VK_RSHIFT ||
               key == VK_MENU || key == VK_LMENU || key == VK_RMENU;
    };

    auto addMainKey = [&](const std::vector<DWORD>& keys) {
        if (keys.empty()) return;
        DWORD mainKey = keys.back();
        g_hotkeyMainKeys.insert(mainKey);

        // For modifier keys, also add the generic version since Windows sends
        if (mainKey == VK_LCONTROL || mainKey == VK_RCONTROL) {
            g_hotkeyMainKeys.insert(VK_CONTROL);
        } else if (mainKey == VK_CONTROL) {
            g_hotkeyMainKeys.insert(VK_LCONTROL);
            g_hotkeyMainKeys.insert(VK_RCONTROL);
        } else if (mainKey == VK_LSHIFT || mainKey == VK_RSHIFT) {
            g_hotkeyMainKeys.insert(VK_SHIFT);
        } else if (mainKey == VK_SHIFT) {
            g_hotkeyMainKeys.insert(VK_LSHIFT);
            g_hotkeyMainKeys.insert(VK_RSHIFT);
        } else if (mainKey == VK_LMENU || mainKey == VK_RMENU) {
            g_hotkeyMainKeys.insert(VK_MENU);
        } else if (mainKey == VK_MENU) {
            g_hotkeyMainKeys.insert(VK_LMENU);
            g_hotkeyMainKeys.insert(VK_RMENU);
        }
    };

    for (const auto& hotkey : g_config.hotkeys) {
        addMainKey(hotkey.keys);

        for (const auto& alt : hotkey.altSecondaryModes) { addMainKey(alt.keys); }
    }

    for (const auto& sensHotkey : g_config.sensitivityHotkeys) { addMainKey(sensHotkey.keys); }

    addMainKey(g_config.guiHotkey);

    addMainKey(g_config.borderlessHotkey);

    addMainKey(g_config.keyRebinds.toggleHotkey);

    g_hotkeyMainKeys.insert(VK_ESCAPE);

    if (g_config.keyRebinds.enabled) {
        for (const auto& rebind : g_config.keyRebinds.rebinds) {
            if (rebind.enabled && rebind.fromKey != 0) {
                g_hotkeyMainKeys.insert(rebind.fromKey);

                // Windows may deliver VK_SHIFT in wParam (and vice-versa).
                if (rebind.fromKey == VK_LCONTROL || rebind.fromKey == VK_RCONTROL) {
                    g_hotkeyMainKeys.insert(VK_CONTROL);
                } else if (rebind.fromKey == VK_CONTROL) {
                    g_hotkeyMainKeys.insert(VK_LCONTROL);
                    g_hotkeyMainKeys.insert(VK_RCONTROL);
                } else if (rebind.fromKey == VK_LSHIFT || rebind.fromKey == VK_RSHIFT) {
                    g_hotkeyMainKeys.insert(VK_SHIFT);
                } else if (rebind.fromKey == VK_SHIFT) {
                    g_hotkeyMainKeys.insert(VK_LSHIFT);
                    g_hotkeyMainKeys.insert(VK_RSHIFT);
                } else if (rebind.fromKey == VK_LMENU || rebind.fromKey == VK_RMENU) {
                    g_hotkeyMainKeys.insert(VK_MENU);
                } else if (rebind.fromKey == VK_MENU) {
                    g_hotkeyMainKeys.insert(VK_LMENU);
                    g_hotkeyMainKeys.insert(VK_RMENU);
                }
            }
        }
    }
}

// This version acquires both required locks - use when you don't already hold them
void RebuildHotkeyMainKeys() {
    std::lock_guard<std::mutex> hotkeyLock(g_hotkeyMainKeysMutex);
    RebuildHotkeyMainKeys_Internal();
}

// Save the original Windows mouse speed setting
void SaveOriginalWindowsMouseSpeed() {
    int currentSpeed = 0;
    if (SystemParametersInfo(SPI_GETMOUSESPEED, 0, &currentSpeed, 0)) {
        g_originalWindowsMouseSpeed = currentSpeed;
        LogCategory("init", "Saved original Windows mouse speed: " + std::to_string(currentSpeed));
    } else {
        Log("WARNING: Failed to get current Windows mouse speed");
        g_originalWindowsMouseSpeed = 10;
    }
}

// Apply the configured Windows mouse speed (if enabled)
void ApplyWindowsMouseSpeed() {
    int targetSpeed = g_config.windowsMouseSpeed;

    if (targetSpeed == 0) {
        if (g_windowsMouseSpeedApplied.load()) {
            if (SystemParametersInfo(SPI_SETMOUSESPEED, 0, reinterpret_cast<void*>(static_cast<intptr_t>(g_originalWindowsMouseSpeed)),
                                     0)) {
                Log("Restored Windows mouse speed to: " + std::to_string(g_originalWindowsMouseSpeed));
            }
            g_windowsMouseSpeedApplied.store(false);
        }
        return;
    }

    if (targetSpeed < 1) targetSpeed = 1;
    if (targetSpeed > 20) targetSpeed = 20;

    if (SystemParametersInfo(SPI_SETMOUSESPEED, 0, reinterpret_cast<void*>(static_cast<intptr_t>(targetSpeed)), 0)) {
        g_windowsMouseSpeedApplied.store(true);
        Log("Applied Windows mouse speed: " + std::to_string(targetSpeed));
    } else {
        Log("WARNING: Failed to set Windows mouse speed to: " + std::to_string(targetSpeed));
    }
}

// Restore the original Windows mouse speed on shutdown
void RestoreWindowsMouseSpeed() {
    if (g_windowsMouseSpeedApplied.load()) {
        if (SystemParametersInfo(SPI_SETMOUSESPEED, 0, reinterpret_cast<void*>(static_cast<intptr_t>(g_originalWindowsMouseSpeed)), 0)) {
            Log("Restored Windows mouse speed to: " + std::to_string(g_originalWindowsMouseSpeed));
        } else {
            Log("WARNING: Failed to restore Windows mouse speed");
        }
        g_windowsMouseSpeedApplied.store(false);
    }
}

void SaveOriginalKeyRepeatSettings() {
    g_originalFilterKeys.cbSize = sizeof(FILTERKEYS);
    if (SystemParametersInfo(SPI_GETFILTERKEYS, sizeof(FILTERKEYS), &g_originalFilterKeys, 0)) {
        g_originalFilterKeysCaptured.store(true);
        LogCategory("init", "Saved original FILTERKEYS: flags=0x" + std::to_string(g_originalFilterKeys.dwFlags) +
                                ", iDelayMSec=" + std::to_string(g_originalFilterKeys.iDelayMSec) +
                                ", iRepeatMSec=" + std::to_string(g_originalFilterKeys.iRepeatMSec));
    } else {
        Log("WARNING: Failed to get current FILTERKEYS settings");
        g_originalFilterKeys.dwFlags = 0;
        g_originalFilterKeys.iDelayMSec = 0;
        g_originalFilterKeys.iRepeatMSec = 0;
        g_originalFilterKeysCaptured.store(false);
    }
}

void ApplyKeyRepeatSettings() {
    if (!g_originalFilterKeysCaptured.load(std::memory_order_acquire)) { SaveOriginalKeyRepeatSettings(); }

    int startDelay = g_config.keyRepeatStartDelay;
    int repeatDelay = g_config.keyRepeatDelay;

    if (startDelay == 0 && repeatDelay == 0) {
        if (g_filterKeysApplied.load()) {
            if (SystemParametersInfo(SPI_SETFILTERKEYS, sizeof(FILTERKEYS), &g_originalFilterKeys, 0)) {
                Log("Restored original FILTERKEYS settings");
            }
            g_filterKeysApplied.store(false);
        }
        return;
    }

    if (startDelay < 0) startDelay = 0;
    if (startDelay > 500) startDelay = 500;
    if (repeatDelay < 0) repeatDelay = 0;
    if (repeatDelay > 500) repeatDelay = 500;

    FILTERKEYS fk = { sizeof(FILTERKEYS) };
    fk.dwFlags = FKF_FILTERKEYSON;
    fk.iWaitMSec = 0;
    fk.iDelayMSec = (startDelay > 0) ? startDelay : g_originalFilterKeys.iDelayMSec;
    fk.iRepeatMSec = (repeatDelay > 0) ? repeatDelay : g_originalFilterKeys.iRepeatMSec;
    fk.iBounceMSec = 0;

    if (SystemParametersInfo(SPI_SETFILTERKEYS, sizeof(FILTERKEYS), &fk, 0)) {
        g_filterKeysApplied.store(true);
        Log("Applied key repeat settings: startDelay=" + std::to_string(fk.iDelayMSec) +
            "ms, repeatDelay=" + std::to_string(fk.iRepeatMSec) + "ms");
    } else {
        Log("WARNING: Failed to set key repeat settings");
    }
}

void RestoreKeyRepeatSettings() {
    if (g_filterKeysApplied.load()) {
        if (SystemParametersInfo(SPI_SETFILTERKEYS, sizeof(FILTERKEYS), &g_originalFilterKeys, 0)) {
            Log("Restored original FILTERKEYS settings");
        } else {
            Log("WARNING: Failed to restore FILTERKEYS settings");
        }
        g_filterKeysApplied.store(false);
    }
}

typedef BOOL(WINAPI* WGLSWAPBUFFERS)(HDC);
WGLSWAPBUFFERS owglSwapBuffers = NULL;
WGLSWAPBUFFERS g_owglSwapBuffersThirdParty = NULL;
std::atomic<void*> g_wglSwapBuffersThirdPartyHookTarget{ nullptr };
typedef BOOL(WINAPI* SETCURSORPOSPROC)(int, int);
SETCURSORPOSPROC oSetCursorPos = NULL;
SETCURSORPOSPROC g_oSetCursorPosThirdParty = NULL;
std::atomic<void*> g_setCursorPosThirdPartyHookTarget{ nullptr };
typedef BOOL(WINAPI* CLIPCURSORPROC)(const RECT*);
CLIPCURSORPROC oClipCursor = NULL;
CLIPCURSORPROC g_oClipCursorThirdParty = NULL;
std::atomic<void*> g_clipCursorThirdPartyHookTarget{ nullptr };
typedef HCURSOR(WINAPI* SETCURSORPROC)(HCURSOR);
SETCURSORPROC oSetCursor = NULL;
SETCURSORPROC g_oSetCursorThirdParty = NULL;
std::atomic<void*> g_setCursorThirdPartyHookTarget{ nullptr };
typedef void(WINAPI* GLVIEWPORTPROC)(GLint x, GLint y, GLsizei width, GLsizei height);
GLVIEWPORTPROC oglViewport = NULL;

// Additional glViewport hook chains for compatibility with driver-level entrypoints and
GLVIEWPORTPROC g_oglViewportDriver = NULL;
GLVIEWPORTPROC g_oglViewportThirdParty = NULL;
std::atomic<void*> g_glViewportDriverHookTarget{ nullptr };
std::atomic<void*> g_glViewportThirdPartyHookTarget{ nullptr };

// Thread-local flag to track if glViewport is being called from our own code
thread_local bool g_internalViewportCall = false;

std::atomic<int> g_glViewportHookCount{ 0 };
std::atomic<bool> g_glViewportHookedViaGLEW{ false };
std::atomic<bool> g_glViewportHookedViaWGL{ false };

typedef void(WINAPI* GLBLITNAMEDFRAMEBUFFERPROC)(GLuint readFramebuffer, GLuint drawFramebuffer, GLint srcX0, GLint srcY0, GLint srcX1,
                                                 GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask,
                                                 GLenum filter);
GLBLITNAMEDFRAMEBUFFERPROC oglBlitNamedFramebuffer = NULL;

typedef void(APIENTRY* PFNGLBLITFRAMEBUFFERPROC_HOOK)(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0,
                                                      GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter);
PFNGLBLITFRAMEBUFFERPROC_HOOK oglBlitFramebuffer = NULL;
std::atomic<bool> g_glBlitFramebufferHooked{ false };

typedef void (*GLFWSETINPUTMODE)(void* window, int mode, int value);
GLFWSETINPUTMODE oglfwSetInputMode = NULL;
GLFWSETINPUTMODE g_oglfwSetInputModeThirdParty = NULL;
std::atomic<void*> g_glfwSetInputModeThirdPartyHookTarget{ nullptr };

typedef UINT(WINAPI* GETRAWINPUTDATAPROC)(HRAWINPUT hRawInput, UINT uiCommand, LPVOID pData, PUINT pcbSize, UINT cbSizeHeader);
GETRAWINPUTDATAPROC oGetRawInputData = NULL;
GETRAWINPUTDATAPROC g_oGetRawInputDataThirdParty = NULL;
std::atomic<void*> g_getRawInputDataThirdPartyHookTarget{ nullptr };

static BOOL ClipCursorHook_Impl(CLIPCURSORPROC next, const RECT* lpRect) {
    if (!next) return FALSE;

    if (g_showGui.load()) { return next(NULL); }

    if (g_gameVersion >= GameVersion(1, 13, 0)) { return next(lpRect); }

    if (g_config.allowCursorEscape) {
        return next(NULL);
    }
    return next(lpRect);
}

BOOL WINAPI hkClipCursor(const RECT* lpRect) { return ClipCursorHook_Impl(oClipCursor, lpRect); }

BOOL WINAPI hkClipCursor_ThirdParty(const RECT* lpRect) {
    CLIPCURSORPROC next = oClipCursor;
    if (g_config.hookChainingNextTarget == HookChainingNextTarget::LatestHook) {
        next = g_oClipCursorThirdParty ? g_oClipCursorThirdParty : oClipCursor;
    }
    return ClipCursorHook_Impl(next, lpRect);
}

static HCURSOR SetCursorHook_Impl(SETCURSORPROC next, HCURSOR hCursor) {
    if (!next) return NULL;

    if (g_gameVersion >= GameVersion(1, 13, 0)) { return next(hCursor); }

    std::string localGameState = g_gameStateBuffers[g_currentGameStateIndex.load(std::memory_order_acquire)];

    if (g_showGui.load()) {
        const CursorTextures::CursorData* cursorData = CursorTextures::GetSelectedCursor(localGameState, 64);
        if (cursorData && cursorData->hCursor) { return next(cursorData->hCursor); }
    }

    if (g_specialCursorHandle.load() != NULL) { return next(hCursor); }

    ICONINFO ii = { sizeof(ICONINFO) };
    if (GetIconInfo(hCursor, &ii)) {
        BITMAP bitmask = {};
        GetObject(ii.hbmMask, sizeof(BITMAP), &bitmask);

        std::string maskHash = "N/A";
        if (bitmask.bmWidth > 0 && bitmask.bmHeight > 0) {
            size_t bufferSize = bitmask.bmWidth * bitmask.bmHeight;
            std::vector<BYTE> maskPixels(bufferSize, 0);
            if (GetBitmapBits(ii.hbmMask, static_cast<LONG>(bufferSize), maskPixels.data()) > 0) {
                uint32_t hash = 0;
                for (BYTE pixel : maskPixels) { hash = ((hash << 5) + hash) ^ pixel; }
                std::ostringstream oss;
                oss << std::hex << hash;
                maskHash = oss.str();
            }
        }

        Log("hkSetCursor: maskHash = " + maskHash);

        if (maskHash == "773ff800") {
            Log("hkSetCursor: Detected special cursor (maskHash=773ff800), caching for later use");
            g_specialCursorHandle.store(hCursor);
        }

        if (ii.hbmMask) DeleteObject(ii.hbmMask);
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
    }

    return next(hCursor);
}

HCURSOR WINAPI hkSetCursor(HCURSOR hCursor) { return SetCursorHook_Impl(oSetCursor, hCursor); }

HCURSOR WINAPI hkSetCursor_ThirdParty(HCURSOR hCursor) {
    SETCURSORPROC next = oSetCursor;
    if (g_config.hookChainingNextTarget == HookChainingNextTarget::LatestHook) {
        next = g_oSetCursorThirdParty ? g_oSetCursorThirdParty : oSetCursor;
    }
    return SetCursorHook_Impl(next, hCursor);
}
// Note: OBS capture is now handled by obs_thread.cpp via glBlitFramebuffer hook

static int lastViewportW = 0;
static int lastViewportH = 0;

static bool GetLatestViewportForHook(int& outModeW, int& outModeH, bool& outStretchEnabled, int& outStretchX, int& outStretchY,
                                     int& outStretchW, int& outStretchH) {
    auto cfgSnap = GetConfigSnapshot();
    if (!cfgSnap) { return false; }

    const int modeIdx = g_currentModeIdIndex.load(std::memory_order_acquire);
    const std::string currentModeId = g_modeIdBuffers[modeIdx];
    const ModeConfig* mode = GetModeFromSnapshot(*cfgSnap, currentModeId);
    if (!mode) { return false; }

    const int screenW = (std::max)(1, GetCachedWindowWidth());
    const int screenH = (std::max)(1, GetCachedWindowHeight());

    // Single source of truth: logic-thread-recalculated mode dimensions.
    // Do not re-run relative/expression math in the hook; that can introduce
    // independent rounding/timing drift versus WM_SIZE enforcement.
    int modeW = mode->width;
    int modeH = mode->height;

    if (modeW < 1 || modeH < 1) { return false; }

    outModeW = modeW;
    outModeH = modeH;

    outStretchEnabled = mode->stretch.enabled;
    if (mode->stretch.enabled) {
        outStretchX = mode->stretch.x;
        outStretchY = mode->stretch.y;
        outStretchW = mode->stretch.width;
        outStretchH = mode->stretch.height;
    } else {
        outStretchX = screenW / 2 - modeW / 2;
        outStretchY = screenH / 2 - modeH / 2;
        outStretchW = modeW;
        outStretchH = modeH;
    }

    return true;
}

static inline void ViewportHook_Impl(GLVIEWPORTPROC next, GLint x, GLint y, GLsizei width, GLsizei height) {
    if (!next) return;

    if (g_internalViewportCall) {
        return next(x, y, width, height);
    }

    // Lock-free read of transition snapshot
    const ViewportTransitionSnapshot& transitionSnap =
        g_viewportTransitionSnapshots[g_viewportTransitionSnapshotIndex.load(std::memory_order_acquire)];
    bool isTransitionActive = transitionSnap.active;

    // Lock-free read of cached mode viewport data (updated by logic_thread)
    const CachedModeViewport& cachedMode = g_viewportModeCache[g_viewportModeCacheIndex.load(std::memory_order_acquire)];

    // The snapshot is updated synchronously on mode switch, while cache has ~16ms lag.
    int modeWidth, modeHeight;
    bool stretchEnabled;
    int stretchX, stretchY, stretchWidth, stretchHeight;

    if (isTransitionActive) {
        modeWidth = transitionSnap.toNativeWidth;
        modeHeight = transitionSnap.toNativeHeight;
        stretchEnabled = true;
        stretchX = transitionSnap.toX;
        stretchY = transitionSnap.toY;
        stretchWidth = transitionSnap.toWidth;
        stretchHeight = transitionSnap.toHeight;
    } else if (GetLatestViewportForHook(modeWidth, modeHeight, stretchEnabled, stretchX, stretchY, stretchWidth, stretchHeight)) {
        // Use live, recalculated dimensions so WM_SIZE-driven relative/expression updates
        // are reflected immediately even before the periodic viewport cache refresh.
    } else if (cachedMode.valid) {
        modeWidth = cachedMode.width;
        modeHeight = cachedMode.height;
        stretchEnabled = cachedMode.stretchEnabled;
        stretchX = cachedMode.stretchX;
        stretchY = cachedMode.stretchY;
        stretchWidth = cachedMode.stretchWidth;
        stretchHeight = cachedMode.stretchHeight;
    } else {
        return next(x, y, width, height);
    }

    bool posValid = x == 0 && y == 0;
    bool widthMatches = (width == modeWidth) || (width == lastViewportW);
    bool heightMatches = (height == modeHeight) || (height == lastViewportH);

    if (isTransitionActive && (!widthMatches || !heightMatches)) {
        widthMatches = widthMatches || (width == transitionSnap.fromNativeWidth) || (width == transitionSnap.toNativeWidth);
        heightMatches = heightMatches || (height == transitionSnap.fromNativeHeight) || (height == transitionSnap.toNativeHeight);
    }

    if (!posValid || !widthMatches || !heightMatches) {
        /*Log("Returning because viewport parameters don't match mode (x=" + std::to_string(x) + ", y=" + std::to_string(y) +
            ", width=" + std::to_string(width) + ", height=" + std::to_string(height) +
            "), lastViewportW=" + std::to_string(lastViewportW) + ", lastViewportH=" + std::to_string(lastViewportH) +
            ", modeWidth=" + std::to_string(modeWidth) + ", modeHeight=" + std::to_string(modeHeight) + ")");*/
        return next(x, y, width, height);
    }

    GLint readFBO = 0;
    GLint currentTexture = 0;

    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFBO);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &currentTexture);

    if (currentTexture == 0 || readFBO != 0) {
        return next(x, y, width, height);
    }

    lastViewportW = modeWidth;
    lastViewportH = modeHeight;

    const int screenW = GetCachedWindowWidth();
    const int screenH = GetCachedWindowHeight();
    if (screenW <= 0 || screenH <= 0) { return next(x, y, width, height); }

    // Check if mode transition animation is active (from snapshot - no lock needed)
    bool useAnimatedDimensions = transitionSnap.active;
    bool isMoveTransition = transitionSnap.isBounceTransition;
    int animatedX = transitionSnap.currentX;
    int animatedY = transitionSnap.currentY;
    int animatedWidth = transitionSnap.currentWidth;
    int animatedHeight = transitionSnap.currentHeight;
    int targetX = transitionSnap.toX;
    int targetY = transitionSnap.toY;
    int targetWidth = transitionSnap.toWidth;
    int targetHeight = transitionSnap.toHeight;

    if (useAnimatedDimensions) {
        bool shouldSkipAnimation = g_config.hideAnimationsInGame;

        if (shouldSkipAnimation) {
            stretchX = targetX;
            stretchY = targetY;
            stretchWidth = targetWidth;
            stretchHeight = targetHeight;
        } else {
            stretchX = animatedX;
            stretchY = animatedY;
            stretchWidth = animatedWidth;
            stretchHeight = animatedHeight;
        }
    } else {
        if (!stretchEnabled) {
            stretchX = screenW / 2 - modeWidth / 2;
            stretchY = screenH / 2 - modeHeight / 2;
            stretchWidth = modeWidth;
            stretchHeight = modeHeight;
        }
    }

    // Convert Y coordinate from Windows screen space (top-left origin) to OpenGL viewport space (bottom-left origin)
    int stretchY_gl = screenH - stretchY - stretchHeight;
    /*Log("Applying viewport hook with parameters: x=" + std::to_string(stretchX) + ", y=" + std::to_string(stretchY_gl) +
        ", width=" + std::to_string(stretchWidth) + ", height=" + std::to_string(stretchHeight) +
        ", modeWidth=" + std::to_string(modeWidth) + ", modeHeight=" + std::to_string(modeHeight) +
        ", screenW=" + std::to_string(screenW) + ", screenH=" + std::to_string(screenH) +
        (useAnimatedDimensions ? ", animated" : ""));*/

    return next(stretchX, stretchY_gl, stretchWidth, stretchHeight);
}

void WINAPI hkglViewport(GLint x, GLint y, GLsizei width, GLsizei height) { ViewportHook_Impl(oglViewport, x, y, width, height); }

// Driver-level glViewport hook (wglGetProcAddress / GLEW-resolved function pointer).
void WINAPI hkglViewport_Driver(GLint x, GLint y, GLsizei width, GLsizei height) {
    ViewportHook_Impl(g_oglViewportDriver, x, y, width, height);
}

void WINAPI hkglViewport_ThirdParty(GLint x, GLint y, GLsizei width, GLsizei height) {
    GLVIEWPORTPROC next = oglViewport ? oglViewport : g_oglViewportDriver;
    if (g_config.hookChainingNextTarget == HookChainingNextTarget::LatestHook) {
        next = g_oglViewportThirdParty ? g_oglViewportThirdParty : (oglViewport ? oglViewport : g_oglViewportDriver);
    }
    ViewportHook_Impl(next, x, y, width, height);
}

void WINAPI hkglBlitNamedFramebuffer(GLuint readFramebuffer, GLuint drawFramebuffer, GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1,
                                     GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter);

static void AttemptHookGlBlitNamedFramebufferViaGlew() {
    static std::atomic<bool> s_hooked{ false };
    if (s_hooked.load(std::memory_order_acquire)) return;
    if (oglBlitNamedFramebuffer != NULL) {
        s_hooked.store(true, std::memory_order_release);
        return;
    }

    PFNGLBLITNAMEDFRAMEBUFFERPROC pFunc = glBlitNamedFramebuffer;
    if (pFunc == NULL) return;

    MH_STATUS st = MH_CreateHook(reinterpret_cast<void*>(pFunc), reinterpret_cast<void*>(&hkglBlitNamedFramebuffer),
                                reinterpret_cast<void**>(&oglBlitNamedFramebuffer));
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) {
        return;
    }
    st = MH_EnableHook(reinterpret_cast<void*>(pFunc));
    if (st != MH_OK && st != MH_ERROR_ENABLED) {
        return;
    }

    s_hooked.store(true, std::memory_order_release);
    LogCategory("init", "Successfully hooked glBlitNamedFramebuffer via GLEW");
}

static BOOL SetCursorPosHook_Impl(SETCURSORPOSPROC next, int X, int Y) {
    if (!next) return FALSE;

    if (g_showGui.load() || g_isShuttingDown.load()) { return next(X, Y); }

    ModeViewportInfo viewport = GetCurrentModeViewport();
    if (!viewport.valid) { return next(X, Y); }

    CapturingState currentState = g_capturingMousePos.load();

    // Convert viewport center (client-space) into absolute screen coordinates.
    int centerX = viewport.stretchX + viewport.stretchWidth / 2;
    int centerY = viewport.stretchY + viewport.stretchHeight / 2;
    int centerX_abs = X;
    int centerY_abs = Y;
    HWND hwnd = g_minecraftHwnd.load();
    RECT clientRectScreen{};
    if (GetWindowClientRectInScreen(hwnd, clientRectScreen)) {
        centerX_abs = clientRectScreen.left + centerX;
        centerY_abs = clientRectScreen.top + centerY;
    } else {
        RECT monRect{};
        if (GetMonitorRectForWindow(hwnd, monRect)) {
            centerX_abs = monRect.left + centerX;
            centerY_abs = monRect.top + centerY;
        }
    }

    if (currentState == CapturingState::DISABLED) {
        g_nextMouseXY.store(std::make_pair(centerX_abs, centerY_abs));
        return next(X, Y);
    }

    if (currentState == CapturingState::NORMAL) {
        auto [expectedX, expectedY] = g_nextMouseXY.load();
        if (expectedX == -1 && expectedY == -1) { return next(X, Y); }
        return next(expectedX, expectedY);
    }

    return next(X, Y);
}

BOOL WINAPI hkSetCursorPos(int X, int Y) { return SetCursorPosHook_Impl(oSetCursorPos, X, Y); }

BOOL WINAPI hkSetCursorPos_ThirdParty(int X, int Y) {
    SETCURSORPOSPROC next = oSetCursorPos;
    if (g_config.hookChainingNextTarget == HookChainingNextTarget::LatestHook) {
        next = g_oSetCursorPosThirdParty ? g_oSetCursorPosThirdParty : oSetCursorPos;
    }
    return SetCursorPosHook_Impl(next, X, Y);
}

#define GLFW_CURSOR 0x00033001
#define GLFW_CURSOR_NORMAL 0x00034001
#define GLFW_CURSOR_HIDDEN 0x00034002
#define GLFW_CURSOR_DISABLED 0x00034003

static void GlfwSetInputModeHook_Impl(GLFWSETINPUTMODE next, void* window, int mode, int value) {
    if (!next) return;
    if (mode != GLFW_CURSOR) { return next(window, mode, value); }

    if (value == GLFW_CURSOR_DISABLED) {
        g_capturingMousePos.store(CapturingState::DISABLED);
        // When GUI is open, don't actually disable/lock the cursor - let it move freely
        if (g_showGui.load()) {
            return; // Skip the call to keep cursor unlocked
        }
        next(window, mode, value);
    } else if (value == GLFW_CURSOR_NORMAL) {
        g_capturingMousePos.store(CapturingState::NORMAL);
        next(window, mode, value);
    } else {
        next(window, mode, value);
    }

    g_capturingMousePos.store(CapturingState::NONE);
}

void hkglfwSetInputMode(void* window, int mode, int value) { GlfwSetInputModeHook_Impl(oglfwSetInputMode, window, mode, value); }

void hkglfwSetInputMode_ThirdParty(void* window, int mode, int value) {
    GLFWSETINPUTMODE next = oglfwSetInputMode;
    if (g_config.hookChainingNextTarget == HookChainingNextTarget::LatestHook) {
        next = g_oglfwSetInputModeThirdParty ? g_oglfwSetInputModeThirdParty : oglfwSetInputMode;
    }
    GlfwSetInputModeHook_Impl(next, window, mode, value);
}

static UINT GetRawInputDataHook_Impl(GETRAWINPUTDATAPROC next, HRAWINPUT hRawInput, UINT uiCommand, LPVOID pData, PUINT pcbSize,
                                    UINT cbSizeHeader) {
    if (!next) return static_cast<UINT>(-1);

    UINT result = next(hRawInput, uiCommand, pData, pcbSize, cbSizeHeader);

    g_wmMouseMoveCount.store(0);

    if (result == static_cast<UINT>(-1) || pData == nullptr || uiCommand != RID_INPUT) { return result; }

    if (g_showGui.load() || g_isShuttingDown.load()) { return result; }

    RAWINPUT* raw = reinterpret_cast<RAWINPUT*>(pData);

    if (raw->header.dwType == RIM_TYPEMOUSE) {
        // Get sensitivity setting using LOCK-FREE access to avoid input delay
        float sensitivityX = 1.0f;
        float sensitivityY = 1.0f;
        bool sensitivityDetermined = false;

        {
            std::lock_guard<std::mutex> lock(g_tempSensitivityMutex);
            if (g_tempSensitivityOverride.active) {
                sensitivityX = g_tempSensitivityOverride.sensitivityX;
                sensitivityY = g_tempSensitivityOverride.sensitivityY;
                sensitivityDetermined = true;
            }
        }

        if (!sensitivityDetermined) {
            // Lock-free read: check transition snapshot first
            const ViewportTransitionSnapshot& transitionSnap =
                g_viewportTransitionSnapshots[g_viewportTransitionSnapshotIndex.load(std::memory_order_acquire)];

            std::string modeId;
            if (transitionSnap.active) {
                modeId = transitionSnap.toModeId; // Target mode during transition (lock-free from snapshot)
            } else {
                // Lock-free read of current mode ID from double-buffer
                modeId = g_modeIdBuffers[g_currentModeIdIndex.load(std::memory_order_acquire)];
            }

            // Check if the mode has a sensitivity override (use snapshot for thread safety)
            auto inputCfgSnap = GetConfigSnapshot();
            const ModeConfig* mode = inputCfgSnap ? GetModeFromSnapshot(*inputCfgSnap, modeId) : nullptr;
            if (mode && mode->sensitivityOverrideEnabled) {
                if (mode->separateXYSensitivity) {
                    sensitivityX = mode->modeSensitivityX;
                    sensitivityY = mode->modeSensitivityY;
                } else {
                    sensitivityX = mode->modeSensitivity;
                    sensitivityY = mode->modeSensitivity;
                }
            } else if (inputCfgSnap) {
                sensitivityX = inputCfgSnap->mouseSensitivity;
                sensitivityY = inputCfgSnap->mouseSensitivity;
            }
        }

        if (!(raw->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
            static float xAccum = 0.0f;
            static float yAccum = 0.0f;
            static float lastSensitivityX = 1.0f;
            static float lastSensitivityY = 1.0f;
            static bool hasLastSensitivity = false;

            const bool sensitivityChanged = !hasLastSensitivity || (std::fabs(sensitivityX - lastSensitivityX) > 0.000001f) ||
                                          (std::fabs(sensitivityY - lastSensitivityY) > 0.000001f);
            if (sensitivityChanged) {
                xAccum = 0.0f;
                yAccum = 0.0f;
                lastSensitivityX = sensitivityX;
                lastSensitivityY = sensitivityY;
                hasLastSensitivity = true;
            }

            const LONG rawX = raw->data.mouse.lLastX;
            const LONG rawY = raw->data.mouse.lLastY;

            // Prevent stale remainder from one direction delaying opposite-direction output.
            if (rawX != 0 && xAccum != 0.0f && ((rawX > 0) != (xAccum > 0.0f))) { xAccum = 0.0f; }
            if (rawY != 0 && yAccum != 0.0f && ((rawY > 0) != (yAccum > 0.0f))) { yAccum = 0.0f; }

            if (sensitivityX != 1.0f || sensitivityY != 1.0f) {
                xAccum += rawX * sensitivityX;
                yAccum += rawY * sensitivityY;

                float roundedX = std::round(xAccum);
                float roundedY = std::round(yAccum);

                if (roundedX > static_cast<float>(LONG_MAX)) roundedX = static_cast<float>(LONG_MAX);
                if (roundedX < static_cast<float>(LONG_MIN)) roundedX = static_cast<float>(LONG_MIN);
                if (roundedY > static_cast<float>(LONG_MAX)) roundedY = static_cast<float>(LONG_MAX);
                if (roundedY < static_cast<float>(LONG_MIN)) roundedY = static_cast<float>(LONG_MIN);

                LONG outputX = static_cast<LONG>(roundedX);
                LONG outputY = static_cast<LONG>(roundedY);

                xAccum -= static_cast<float>(outputX);
                yAccum -= static_cast<float>(outputY);

                raw->data.mouse.lLastX = outputX;
                raw->data.mouse.lLastY = outputY;
            } else {
                // No scaling: avoid carrying stale fractional remainder across future overrides.
                xAccum = 0.0f;
                yAccum = 0.0f;
            }
        }
    }

    return result;
}

UINT WINAPI hkGetRawInputData(HRAWINPUT hRawInput, UINT uiCommand, LPVOID pData, PUINT pcbSize, UINT cbSizeHeader) {
    return GetRawInputDataHook_Impl(oGetRawInputData, hRawInput, uiCommand, pData, pcbSize, cbSizeHeader);
}

UINT WINAPI hkGetRawInputData_ThirdParty(HRAWINPUT hRawInput, UINT uiCommand, LPVOID pData, PUINT pcbSize, UINT cbSizeHeader) {
    GETRAWINPUTDATAPROC next = oGetRawInputData;
    if (g_config.hookChainingNextTarget == HookChainingNextTarget::LatestHook) {
        next = g_oGetRawInputDataThirdParty ? g_oGetRawInputDataThirdParty : oGetRawInputData;
    }
    return GetRawInputDataHook_Impl(next, hRawInput, uiCommand, pData, pcbSize, cbSizeHeader);
}

void WINAPI hkglBlitNamedFramebuffer(GLuint readFramebuffer, GLuint drawFramebuffer, GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1,
                                     GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter) {
    if (drawFramebuffer != 0) {
        return oglBlitNamedFramebuffer(readFramebuffer, drawFramebuffer, srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask,
                                       filter);
    }

    ModeViewportInfo viewport = GetCurrentModeViewport();

    if (viewport.valid) {
        int screenH = GetCachedWindowHeight();
        int destY0_screen = screenH - viewport.stretchY - viewport.stretchHeight;
        int destY1_screen = screenH - viewport.stretchY;

        return oglBlitNamedFramebuffer(readFramebuffer, drawFramebuffer, srcX0, srcY0, srcX1, srcY1, viewport.stretchX, destY0_screen,
                                       viewport.stretchX + viewport.stretchWidth, destY1_screen, mask, filter);
    }

    return oglBlitNamedFramebuffer(readFramebuffer, drawFramebuffer, srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask, filter);
}

void AttemptAggressiveGlViewportHook() {
    int hooksCreated = 0;

    // We only install ONE additional driver-level hook target at a time.
    if (g_glViewportDriverHookTarget.load(std::memory_order_acquire) != nullptr) {
        return;
    }

    // Strategy 1 (preferred): Hook via wglGetProcAddress (driver-specific implementation)
    if (!g_glViewportHookedViaWGL.load()) {
        typedef PROC(WINAPI * PFN_wglGetProcAddress)(LPCSTR);
        HMODULE hOpenGL32 = GetModuleHandle(L"opengl32.dll");
        if (hOpenGL32) {
            PFN_wglGetProcAddress pwglGetProcAddress =
                reinterpret_cast<PFN_wglGetProcAddress>(GetProcAddress(hOpenGL32, "wglGetProcAddress"));
            if (pwglGetProcAddress) {
                PROC pGlViewportWGL = pwglGetProcAddress("glViewport");
                if (pGlViewportWGL != NULL && reinterpret_cast<void*>(pGlViewportWGL) != reinterpret_cast<void*>(&hkglViewport) &&
                    reinterpret_cast<void*>(pGlViewportWGL) != reinterpret_cast<void*>(&hkglViewport_Driver) &&
                    reinterpret_cast<void*>(pGlViewportWGL) != reinterpret_cast<void*>(oglViewport)) {
                    Log("Attempting glViewport hook via wglGetProcAddress: " + std::to_string(reinterpret_cast<uintptr_t>(pGlViewportWGL)));
                    GLVIEWPORTPROC pViewportFunc = reinterpret_cast<GLVIEWPORTPROC>(pGlViewportWGL);
                    if (HookChain::TryCreateAndEnableHook(reinterpret_cast<void*>(pViewportFunc), reinterpret_cast<void*>(&hkglViewport_Driver),
                                               reinterpret_cast<void**>(&g_oglViewportDriver), "glViewport (wglGetProcAddress)")) {
                        g_glViewportHookedViaWGL.store(true);
                        g_glViewportDriverHookTarget.store(reinterpret_cast<void*>(pViewportFunc), std::memory_order_release);
                        hooksCreated++;
                        Log("SUCCESS: glViewport hooked via wglGetProcAddress (driver target)");
                    }
                }
            }
        }
    }

    if (g_glViewportDriverHookTarget.load(std::memory_order_acquire) == nullptr && !g_glViewportHookedViaGLEW.load()) {
        GLVIEWPORTPROC pGlViewportGLEW = glViewport;
        if (pGlViewportGLEW != NULL && reinterpret_cast<void*>(pGlViewportGLEW) != reinterpret_cast<void*>(&hkglViewport) &&
            reinterpret_cast<void*>(pGlViewportGLEW) != reinterpret_cast<void*>(&hkglViewport_Driver) &&
            reinterpret_cast<void*>(pGlViewportGLEW) != reinterpret_cast<void*>(oglViewport)) {
            Log("Attempting glViewport hook via GLEW pointer: " + std::to_string(reinterpret_cast<uintptr_t>(pGlViewportGLEW)));
            if (HookChain::TryCreateAndEnableHook(reinterpret_cast<void*>(pGlViewportGLEW), reinterpret_cast<void*>(&hkglViewport_Driver),
                                       reinterpret_cast<void**>(&g_oglViewportDriver), "glViewport (GLEW pointer)")) {
                g_glViewportHookedViaGLEW.store(true);
                g_glViewportDriverHookTarget.store(reinterpret_cast<void*>(pGlViewportGLEW), std::memory_order_release);
                hooksCreated++;
                Log("SUCCESS: glViewport hooked via GLEW pointer (driver target)");
            }
        }
    }

    g_glViewportHookCount.fetch_add(hooksCreated);
    Log("Aggressive glViewport hooking complete. Total additional hooks created: " + std::to_string(hooksCreated));
    Log("Total glViewport hook count: " + std::to_string(g_glViewportHookCount.load()));
}


GLuint CalculateGameTextureId() {
    ModeViewportInfo viewport = GetCurrentModeViewport();
    if (!viewport.valid) {
        Log("CalculateGameTextureId: Invalid viewport, cannot calculate texture ID");
        return UINT_MAX;
    }

    int targetWidth = viewport.width;
    int targetHeight = viewport.height;

    Log("CalculateGameTextureId: Looking for texture with dimensions " + std::to_string(targetWidth) + "x" + std::to_string(targetHeight));

    constexpr GLuint kMaxCheckRange = 1000;

    std::array<uint8_t, kMaxCheckRange> excludedTextureMask{};
    size_t excludedTextureCount = 0;
    auto markExcludedTexture = [&](GLuint textureId) {
        if (textureId != 0 && textureId != UINT_MAX && textureId < kMaxCheckRange && excludedTextureMask[textureId] == 0) {
            excludedTextureMask[textureId] = 1;
            ++excludedTextureCount;
        }
    };

    {
        GLuint obsOverrideTexture = g_obsOverrideTexture.load(std::memory_order_acquire);
        markExcludedTexture(obsOverrideTexture);

        GLuint obsCaptureTexture = GetObsCaptureTexture();
        markExcludedTexture(obsCaptureTexture);

        std::vector<GLuint> renderThreadExcludedTextureIds;
        GetRenderThreadCalibrationExcludeTextureIds(renderThreadExcludedTextureIds);
        for (GLuint id : renderThreadExcludedTextureIds) { markExcludedTexture(id); }
    }

    if (excludedTextureCount > 0) {
        Log("CalculateGameTextureId: Excluding " + std::to_string(excludedTextureCount) +
            " internal texture IDs from calibration");
    }

    GLint oldTexture = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture);

    for (GLuint texId = 0; texId < kMaxCheckRange; texId++) {
        if (excludedTextureMask[texId] != 0) { continue; }

        if (!glIsTexture(texId)) { continue; }

        glBindTexture(GL_TEXTURE_2D, texId);

        GLint width = 0, height = 0;
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);

        if (width == targetWidth && height == targetHeight) {
            // Check texture parameters: minFilter and magFilter must be GL_NEAREST,
            // wrapS and wrapT must be GL_CLAMP_TO_EDGE
            GLint minFilter = 0, magFilter = 0, wrapS = 0, wrapT = 0;
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &minFilter);
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &magFilter);
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, &wrapS);
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, &wrapT);

            if (g_gameVersion <= GameVersion(1, 16, 5)) {
                if (minFilter != GL_NEAREST || magFilter != GL_NEAREST || wrapS != GL_CLAMP || wrapT != GL_CLAMP) {
                    Log("CalculateGameTextureId: Texture " + std::to_string(texId) +
                        " has matching dimensions but wrong parameters (minFilter=" + std::to_string(minFilter) + ", magFilter=" +
                        std::to_string(magFilter) + ", wrapS=" + std::to_string(wrapS) + ", wrapT=" + std::to_string(wrapT) + ")");
                    continue;
                }
            } else {
                /*
                if (minFilter != GL_NEAREST || magFilter != GL_NEAREST || wrapS != GL_CLAMP || wrapT != GL_CLAMP) {
                    Log("CalculateGameTextureId: Texture " + std::to_string(texId) +
                        " has matching dimensions but wrong parameters (minFilter=" + std::to_string(minFilter) + ", magFilter=" +
                        std::to_string(magFilter) + ", wrapS=" + std::to_string(wrapS) + ", wrapT=" + std::to_string(wrapT) + ")");
                    continue;
                }*/
            }

            Log("CalculateGameTextureId: Found matching texture ID " + std::to_string(texId) + " with dimensions " + std::to_string(width) +
                "x" + std::to_string(height));
            glBindTexture(GL_TEXTURE_2D, oldTexture);
            return texId;
        }

        glBindTexture(GL_TEXTURE_2D, oldTexture);
    }

    Log("CalculateGameTextureId: No matching texture found in range 1-" + std::to_string(kMaxCheckRange));
    return UINT_MAX;
}

static BOOL SwapBuffersHook_Impl(WGLSWAPBUFFERS next, HDC hDc) {
    if (!next) return FALSE;
    auto startTime = std::chrono::high_resolution_clock::now();
    _set_se_translator(SEHTranslator);

    try {
        if (!g_glewLoaded) {
            PROFILE_SCOPE_CAT("GLEW Initialization", "SwapBuffers");
            glewExperimental = GL_TRUE;
            if (glewInit() == GLEW_OK) {
                LogCategory("init", "[RENDER] GLEW Initialized successfully.");
                g_glewLoaded = true;

                g_lastSeenGameGLContext.store(wglGetCurrentContext(), std::memory_order_release);

                g_welcomeToastVisible.store(true);

                CursorTextures::LoadCursorTextures();

                // Initialize shared OpenGL contexts for all worker threads (render, mirror)
                // This must be done BEFORE any thread starts to ensure all contexts are in the same share group
                HGLRC currentContext = wglGetCurrentContext();
                if (currentContext) {
                    if (InitializeSharedContexts(currentContext, hDc)) {
                        LogCategory("init", "[RENDER] Shared contexts initialized - GPU texture sharing enabled for all threads");
                    } else {
                        Log("[RENDER] Shared context initialization failed - starting worker threads in fallback mode");
                    }

                    // ALWAYS start worker threads. They will automatically use the pre-shared contexts if available,
                    StartRenderThread(currentContext);
                    StartMirrorCaptureThread(currentContext);
                    StartObsHookThread();
                }

                AttemptAggressiveGlViewportHook();

                AttemptHookGlBlitNamedFramebufferViaGlew();

                // Note: glBlitFramebuffer hook for OBS is now handled by obs_thread.cpp
            } else {
                Log("[RENDER] ERROR: Failed to initialize GLEW.");
                return next(hDc);
            }
        }
        if (g_isShuttingDown.load()) { return next(hDc); }

        {
            static std::chrono::steady_clock::time_point s_lastViewportCompatCheck = std::chrono::steady_clock::now();
            auto now = std::chrono::steady_clock::now();
            if (now - s_lastViewportCompatCheck > std::chrono::milliseconds(2000)) {
                HookChain::RefreshAllThirdPartyHookChains();
                s_lastViewportCompatCheck = now;
            }
        }

        {
            HGLRC currentContext = wglGetCurrentContext();
            HGLRC lastContext = g_lastSeenGameGLContext.load(std::memory_order_acquire);
            const uint64_t nowMs = GetTickCount64();

            // Some environments (mods/overlays/drivers) can temporarily switch WGL contexts or alternate between
            // multiple contexts. Restarting shared contexts/threads immediately in that case can cause severe lag
            // and make mirrors/overlays appear to "stop" (threads never stabilize). Debounce restarts.
            static HGLRC s_pendingContext = NULL;
            static uint64_t s_pendingSinceMs = 0;
            static uint64_t s_lastRestartMs = 0;
            constexpr uint64_t kContextStableMs = 250;
            constexpr uint64_t kMinRestartIntervalMs = 2000;

            const bool contextChanged = (currentContext && lastContext && currentContext != lastContext);
            if (contextChanged) {
                if (s_pendingContext != currentContext) {
                    s_pendingContext = currentContext;
                    s_pendingSinceMs = nowMs;
                }

                const bool stableLongEnough = (s_pendingSinceMs != 0) && ((nowMs - s_pendingSinceMs) >= kContextStableMs);
                const bool restartAllowed = (s_lastRestartMs == 0) || ((nowMs - s_lastRestartMs) >= kMinRestartIntervalMs);
                if (stableLongEnough && restartAllowed) {
                    Log("[RENDER] Detected stable WGL context change - restarting shared contexts/threads");
                    s_lastRestartMs = nowMs;
                    s_pendingContext = NULL;
                    s_pendingSinceMs = 0;

                    StopObsHookThread();
                    StopMirrorCaptureThread();
                    StopRenderThread();

                    CleanupSharedContexts();

                    if (InitializeSharedContexts(currentContext, hDc)) {
                        Log("[RENDER] Reinitialized shared contexts after context change");
                    } else {
                        Log("[RENDER] Failed to reinitialize shared contexts after context change - restarting threads in fallback mode");
                    }

                    // Restart worker threads regardless of shared-context init success.
                    StartRenderThread(currentContext);
                    StartMirrorCaptureThread(currentContext);
                    StartObsHookThread();

                    g_cachedGameTextureId.store(UINT_MAX, std::memory_order_release);
                    g_lastSeenGameGLContext.store(currentContext, std::memory_order_release);
                }
            } else {
                s_pendingContext = NULL;
                s_pendingSinceMs = 0;
                if (currentContext && (!lastContext)) {
                    g_lastSeenGameGLContext.store(currentContext, std::memory_order_release);
                }
            }

            // Thread health watchdog (rate-limited): if a worker thread crashed/exited, try restarting it.
            // This helps recover from transient driver/ImGui/font/etc crashes that would otherwise leave mirrors black.
            {
                static uint64_t s_lastHealthRestartAttemptMs = 0;
                constexpr uint64_t kHealthRestartIntervalMs = 2000;
                if (currentContext && (s_lastHealthRestartAttemptMs == 0 || (nowMs - s_lastHealthRestartAttemptMs) >= kHealthRestartIntervalMs)) {
                    s_lastHealthRestartAttemptMs = nowMs;

                    // Only restart capture thread when something actually consumes captures.
                    const bool needCaptureForMirrors = (g_activeMirrorCaptureCount.load(std::memory_order_acquire) > 0);
                    const bool needCaptureForEyeZoom = g_showEyeZoom.load(std::memory_order_relaxed) ||
                                                       g_isTransitioningFromEyeZoom.load(std::memory_order_relaxed);
                    const bool needCaptureForObsOrVc = g_graphicsHookDetected.load(std::memory_order_acquire) || IsVirtualCameraActive();
                    const bool needCapture = needCaptureForMirrors || needCaptureForEyeZoom || needCaptureForObsOrVc;

                    if (!g_renderThreadRunning.load(std::memory_order_acquire)) {
                        StartRenderThread(currentContext);
                    }
                    if (needCapture && !g_mirrorCaptureRunning.load(std::memory_order_acquire)) {
                        StartMirrorCaptureThread(currentContext);
                    }
                }
            }
        }

        // Start logic thread if not already running (handles OBS detection, hotkey resets, etc.)
        if (!g_logicThreadRunning.load() && g_configLoaded.load()) { StartLogicThread(); }

        // Early exit if config hasn't been loaded yet (prevents race conditions during startup)
        if (!g_configLoaded.load()) { return next(hDc); }

        auto frameCfgSnap = GetConfigSnapshot();
        if (!frameCfgSnap) { return next(hDc); }
        const Config& frameCfg = *frameCfgSnap;

        HWND hwnd = WindowFromDC(hDc);
        if (!hwnd) { return next(hDc); }
        if (hwnd != g_minecraftHwnd.load()) { g_minecraftHwnd.store(hwnd); }

        // This copy is expensive: it blits the full game texture + inserts fences + flushes.
        {
            const bool needCaptureForMirrors = (g_activeMirrorCaptureCount.load(std::memory_order_acquire) > 0);
            const bool needCaptureForEyeZoom = g_showEyeZoom.load(std::memory_order_relaxed) ||
                                               g_isTransitioningFromEyeZoom.load(std::memory_order_relaxed);
            const bool needCaptureForObsOrVc = g_graphicsHookDetected.load(std::memory_order_acquire) || IsVirtualCameraActive();

            const bool needCapture = needCaptureForMirrors || needCaptureForEyeZoom || needCaptureForObsOrVc;
            if (needCapture) {
                static auto s_lastMirrorOnlyCaptureSubmit = std::chrono::steady_clock::time_point{};
                static int s_lastMirrorOnlyW = 0;
                static int s_lastMirrorOnlyH = 0;
                bool allowCaptureThisFrame = true;

                GLuint gameTexture = g_cachedGameTextureId.load(std::memory_order_acquire);
                if (gameTexture != UINT_MAX) {
                    ModeViewportInfo viewport = GetCurrentModeViewport();
                    if (viewport.valid) {
                        if (needCaptureForMirrors && !needCaptureForEyeZoom && !needCaptureForObsOrVc) {
                            const int maxMirrorFps = g_activeMirrorCaptureMaxFps.load(std::memory_order_acquire);
                            if (maxMirrorFps > 0) {
                                const auto now = std::chrono::steady_clock::now();
                                const double intervalMsD = 1000.0 / static_cast<double>((std::max)(1, maxMirrorFps));
                                const auto interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                    std::chrono::duration<double, std::milli>(intervalMsD));

                                const bool dimsChanged = (viewport.width != s_lastMirrorOnlyW) || (viewport.height != s_lastMirrorOnlyH);

                                if (!dimsChanged && s_lastMirrorOnlyCaptureSubmit.time_since_epoch().count() != 0) {
                                    if ((now - s_lastMirrorOnlyCaptureSubmit) < interval) {
                                        allowCaptureThisFrame = false;
                                    }
                                }

                                if (allowCaptureThisFrame) {
                                    s_lastMirrorOnlyCaptureSubmit = now;
                                    s_lastMirrorOnlyW = viewport.width;
                                    s_lastMirrorOnlyH = viewport.height;
                                }
                            }
                        }

                        // Sync screen/game geometry for capture thread to compute render cache.
                        const int fullW_capture = GetCachedWindowWidth();
                        const int fullH_capture = GetCachedWindowHeight();
                        g_captureScreenW.store(fullW_capture, std::memory_order_release);
                        g_captureScreenH.store(fullH_capture, std::memory_order_release);
                        g_captureGameW.store(viewport.width, std::memory_order_release);
                        g_captureGameH.store(viewport.height, std::memory_order_release);

                        g_captureFinalX.store(viewport.stretchX, std::memory_order_release);
                        g_captureFinalY.store(viewport.stretchY, std::memory_order_release);
                        g_captureFinalW.store(viewport.stretchWidth, std::memory_order_release);
                        g_captureFinalH.store(viewport.stretchHeight, std::memory_order_release);

                        // SubmitFrameCapture already inserts its own fences and flushes after them;
                        // avoid an extra glFlush here (it can reduce FPS by forcing more driver work per frame).
                        if (allowCaptureThisFrame) {
                            SubmitFrameCapture(gameTexture, viewport.width, viewport.height);
                        }
                    }
                }
            }
        }

        // Mark safe capture window - capture thread can now safely read the game texture
        g_safeToCapture.store(true, std::memory_order_release);

        bool shouldCheckSubclass = (g_gameVersion < GameVersion(1, 13, 0)) || (g_originalWndProc == NULL);

        if (shouldCheckSubclass && hwnd != NULL) {
            PROFILE_SCOPE_CAT("Window Subclassing", "SwapBuffers");
            SubclassGameWindow(hwnd);
        }

        {
            bool showTextureGrid = frameCfg.debug.showTextureGrid;
            ModeViewportInfo viewport = GetCurrentModeViewport();
            // Store texture grid state so the render thread can start an ImGui frame for text labels
            g_showTextureGrid.store(showTextureGrid, std::memory_order_relaxed);
            g_textureGridModeWidth.store(viewport.width, std::memory_order_relaxed);
            g_textureGridModeHeight.store(viewport.height, std::memory_order_relaxed);
            if (showTextureGrid && g_glInitialized.load(std::memory_order_acquire) && g_solidColorProgram != 0) {
                PROFILE_SCOPE_CAT("Texture Grid Overlay", "Debug");
                RenderTextureGridOverlay(true, viewport.width, viewport.height);
            }
        }

        const int fullW = GetCachedWindowWidth(), fullH = GetCachedWindowHeight();

        int windowWidth = 0, windowHeight = 0;
        {
            RECT rect;
            if (GetClientRect(hwnd, &rect)) {
                windowWidth = rect.right - rect.left;
                windowHeight = rect.bottom - rect.top;
            }
        }

        constexpr int kFullscreenTolPx = 1;
        const bool hasWindowClientSize = (windowWidth > 0 && windowHeight > 0);
        const bool isWindowedPresentation =
            hasWindowClientSize && (windowWidth < (fullW - kFullscreenTolPx) || windowHeight < (fullH - kFullscreenTolPx));

        if (g_cachedGameTextureId.load() == UINT_MAX) {
            GLint gameTextureId = UINT_MAX;
            {
                PROFILE_SCOPE_CAT("Calculate Game Texture ID", "SwapBuffers");
                gameTextureId = CalculateGameTextureId();
            }
            if (gameTextureId != UINT_MAX) {
                g_cachedGameTextureId.store(gameTextureId);
                Log("Calculated game texture ID: " + std::to_string(gameTextureId));
            }
            // This will retry next frame. We do NOT store UINT_MAX explicitly -
            // render thread has (ready frame / safe read texture fallback) rather
        }

        // Note: Windows mouse speed application is now handled by the logic thread
        // Note: Hotkey secondary mode reset on world exit is now handled by the logic thread

        if (isWindowedPresentation) {
            bool isPre113 = (g_gameVersion < GameVersion(1, 13, 0));
            if (isPre113) {
                ModeViewportInfo viewport = GetCurrentModeViewport();
                int offsetX = 0;
                int offsetY = 0;
                int contentW = windowWidth;
                int contentH = windowHeight;

                if (viewport.valid && viewport.stretchWidth > 0 && viewport.stretchHeight > 0) {
                    offsetX = viewport.stretchX;
                    offsetY = viewport.stretchY;
                    contentW = viewport.stretchWidth;
                    contentH = viewport.stretchHeight;
                } else {
                    offsetX = (fullW - windowWidth) / 2;
                    offsetY = (fullH - windowHeight) / 2;
                }

                g_obsPre113Windowed.store(true, std::memory_order_release);
                g_obsPre113OffsetX.store(offsetX, std::memory_order_release);
                g_obsPre113OffsetY.store(offsetY, std::memory_order_release);
                g_obsPre113ContentW.store(contentW, std::memory_order_release);
                g_obsPre113ContentH.store(contentH, std::memory_order_release);
            } else {
                g_obsPre113Windowed.store(false, std::memory_order_release);
            }
        } else {
            g_obsPre113Windowed.store(false, std::memory_order_release);
        }

        if (g_graphicsHookDetected.load()) {
            EnableObsOverride();
        } else {
            ClearObsOverride();
        }


        if (g_configLoadFailed.load()) {
            Log("Configuration load failed");
            g_safeToCapture.store(false, std::memory_order_release);
            HandleConfigLoadFailed(hDc, next);
            return next(hDc);
        }

        // Lock-free read of current mode ID from double-buffer
        std::string desiredModeId = g_modeIdBuffers[g_currentModeIdIndex.load(std::memory_order_acquire)];

        // Lock-free read of last frame mode ID from double-buffer
        std::string lastFrameModeIdCopy = g_lastFrameModeIdBuffers[g_lastFrameModeIdIndex.load(std::memory_order_acquire)];

        if (IsModeTransitionActive()) {
            g_isTransitioningMode = true;
        } else if (lastFrameModeIdCopy != desiredModeId) {
            PROFILE_SCOPE_CAT("Mode Transition Complete", "SwapBuffers");
            g_isTransitioningMode = true;
            Log("Mode transition detected (no animation): " + lastFrameModeIdCopy + " -> " + desiredModeId);

            int modeWidth = 0, modeHeight = 0;
            bool modeValid = false;
            {
                const ModeConfig* newMode = GetMode(desiredModeId);
                if (newMode) {
                    modeWidth = newMode->width;
                    modeHeight = newMode->height;
                    modeValid = true;
                }
            }
            if (modeValid) { RequestWindowClientResize(hwnd, modeWidth, modeHeight, "swapbuffers:mode_transition_complete"); }
        }

        // Note: Video player update is now done in render_thread

        std::string localGameState = g_gameStateBuffers[g_currentGameStateIndex.load(std::memory_order_acquire)];

        bool showPerformanceOverlay = frameCfg.debug.showPerformanceOverlay;
        bool showProfiler = frameCfg.debug.showProfiler;

        Profiler::GetInstance().SetEnabled(showProfiler);
        if (showProfiler) { Profiler::GetInstance().MarkAsRenderThread(); }

        ModeConfig modeToRenderCopy;
        bool modeFound = false;
        {
            // Use target/desired mode - GetMode is lock-free
            const ModeConfig* tempMode = GetMode(desiredModeId);
            if (!tempMode && g_isTransitioningMode) {
                tempMode = GetMode(lastFrameModeIdCopy);
            }
            if (tempMode) {
                modeToRenderCopy = *tempMode;
                modeFound = true;
            }
        }

        if (!modeFound) {
            Log("ERROR: Could not find mode to render, aborting frame");
            return next(hDc);
        }

        bool isEyeZoom = modeToRenderCopy.id == "EyeZoom";
        bool shouldRenderGui = g_showGui.load();

        bool isTransitioningFromEyeZoom = false;
        int eyeZoomAnimatedViewportX = -1;

        if (IsModeTransitionActive()) {
            ModeTransitionState eyeZoomTransitionState = GetModeTransitionState();
            std::string fromModeId = eyeZoomTransitionState.fromModeId;

            if (!isEyeZoom && fromModeId == "EyeZoom") {
                isTransitioningFromEyeZoom = true;
                eyeZoomAnimatedViewportX = eyeZoomTransitionState.x;
            } else if (isEyeZoom && fromModeId != "EyeZoom") {
                eyeZoomAnimatedViewportX = eyeZoomTransitionState.x;
            }
        }

        // Set global GUI state for render thread to pick up
        g_shouldRenderGui.store(shouldRenderGui, std::memory_order_relaxed);
        g_showPerformanceOverlay.store(showPerformanceOverlay, std::memory_order_relaxed);
        g_showProfiler.store(showProfiler, std::memory_order_relaxed);
        bool hideAnimOnScreenEyeZoom = frameCfg.hideAnimationsInGame;
        bool showEyeZoomOnScreen = isEyeZoom || (isTransitioningFromEyeZoom && !hideAnimOnScreenEyeZoom);
        g_showEyeZoom.store(showEyeZoomOnScreen, std::memory_order_relaxed);
        g_eyeZoomFadeOpacity.store(1.0f, std::memory_order_relaxed);
        g_eyeZoomAnimatedViewportX.store(eyeZoomAnimatedViewportX, std::memory_order_relaxed);
        // Release ensures all preceding EyeZoom stores are visible when the reader acquires this value
        g_isTransitioningFromEyeZoom.store(isTransitioningFromEyeZoom, std::memory_order_release);

        // This must be computed BEFORE any early-exit checks that reference it.
        const bool needsDualRendering = g_graphicsHookDetected.load(std::memory_order_acquire) || IsVirtualCameraActive();

        {
            const bool modeSizesFullscreen = (modeToRenderCopy.width == fullW && modeToRenderCopy.height == fullH);
            const bool stretchIsFullscreen =
                (!modeToRenderCopy.stretch.enabled) ||
                (modeToRenderCopy.stretch.width == fullW && modeToRenderCopy.stretch.height == fullH && modeToRenderCopy.stretch.x == 0 &&
                 modeToRenderCopy.stretch.y == 0);
            const bool borderVisible = modeToRenderCopy.border.enabled && modeToRenderCopy.border.width > 0;

            const bool anyModeOverlaysConfigured =
                (!modeToRenderCopy.mirrorIds.empty() || !modeToRenderCopy.mirrorGroupIds.empty() ||
                 (g_imageOverlaysVisible.load(std::memory_order_acquire) && !modeToRenderCopy.imageIds.empty()) ||
                 (g_windowOverlaysVisible.load(std::memory_order_acquire) && !modeToRenderCopy.windowOverlayIds.empty()));

            const bool anyImGuiOrDebugOverlay = shouldRenderGui || showPerformanceOverlay || showProfiler || showEyeZoomOnScreen ||
                                                frameCfg.debug.showTextureGrid;

            const bool anyOtherCustomOutput = frameCfg.debug.fakeCursor || g_screenshotRequested.load(std::memory_order_relaxed);

            const bool canSkipCustomRender = g_glInitialized.load(std::memory_order_acquire) && !needsDualRendering &&
                                             !IsModeTransitionActive() && modeSizesFullscreen &&
                                             stretchIsFullscreen && !borderVisible && !anyModeOverlaysConfigured &&
                                             !anyImGuiOrDebugOverlay && !anyOtherCustomOutput;

            if (canSkipCustomRender) {

                int targetFPS = frameCfg.fpsLimit;
                if (targetFPS > 0 && g_highResTimer) {
                    PROFILE_SCOPE_CAT("FPS Limit Sleep (Skip Render)", "Timing");

                    const double targetFrameTimeUs = 1000000.0 / targetFPS;
                    const bool isHighFPS = targetFPS > 500;

                    std::lock_guard<std::mutex> lock(g_fpsLimitMutex);
                    auto now = std::chrono::high_resolution_clock::now();
                    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - g_lastFrameEndTime).count();
                    double timeToWaitUs = targetFrameTimeUs - elapsed;

                    if (timeToWaitUs > 0) {
                        if (isHighFPS) {
                            LARGE_INTEGER dueTime;
                            dueTime.QuadPart = -static_cast<LONGLONG>(timeToWaitUs * 10LL);
                            if (SetWaitableTimer(g_highResTimer, &dueTime, 0, NULL, NULL, FALSE)) {
                                WaitForSingleObject(g_highResTimer, 1000);
                            }
                        } else {
                            if (timeToWaitUs > 10) {
                                LARGE_INTEGER dueTime;
                                dueTime.QuadPart = -static_cast<LONGLONG>(timeToWaitUs * 10LL);
                                if (SetWaitableTimer(g_highResTimer, &dueTime, 0, NULL, NULL, FALSE)) {
                                    WaitForSingleObject(g_highResTimer, 1000);
                                }
                            }
                        }
                        g_lastFrameEndTime = g_lastFrameEndTime + std::chrono::microseconds(static_cast<long long>(targetFrameTimeUs));
                    } else {
                        g_lastFrameEndTime = now;
                    }
                }

                if (IsModeTransitionActive()) {
                    PROFILE_SCOPE_CAT("Mode Transition Animation (Skip Render)", "SwapBuffers");
                    UpdateModeTransition();
                }

                if (frameCfg.debug.delayRenderingUntilFinished) { glFinish(); }
                if (frameCfg.debug.delayRenderingUntilBlitted) { WaitForOverlayBlitFence(); }

                auto swapStartTime = std::chrono::high_resolution_clock::now();
                BOOL result = next(hDc);

                g_safeToCapture.store(false, std::memory_order_release);

                auto swapEndTime = std::chrono::high_resolution_clock::now();
                std::chrono::duration<double, std::milli> swapDuration = swapEndTime - swapStartTime;
                g_originalFrameTimeMs = swapDuration.count();

                std::chrono::duration<double, std::milli> fp_ms = swapStartTime - startTime;
                g_lastFrameTimeMs = fp_ms.count();

                {
                    int nextIndex = 1 - g_lastFrameModeIdIndex.load(std::memory_order_relaxed);
                    g_lastFrameModeIdBuffers[nextIndex] = desiredModeId;
                    g_lastFrameModeIdIndex.store(nextIndex, std::memory_order_release);
                    g_lastFrameModeId = desiredModeId;
                }

                return result;
            }
        }

        if (!g_glInitialized.load(std::memory_order_acquire)) {
            PROFILE_SCOPE_CAT("GPU Resource Init Check", "SwapBuffers");
            Log("[RENDER] Conditions met for GPU resource initialization.");
            InitializeGPUResources();

            if (!g_glInitialized.load(std::memory_order_acquire)) {
                Log("FATAL: GPU resource initialization failed. Aborting custom render for this frame.");
                g_safeToCapture.store(false, std::memory_order_release);
                return next(hDc);
            }
        }

        // Note: Game state reset (wall/title/waiting) is now handled by logic_thread

        GLState s;
        {
            PROFILE_SCOPE_CAT("OpenGL State Backup", "SwapBuffers");
            SaveGLState(&s);
        }

        {
            PROFILE_SCOPE_CAT("Texture Cleanup", "SwapBuffers");
            if (g_hasTexturesToDelete.load(std::memory_order_acquire)) {
                std::lock_guard<std::mutex> lock(g_texturesToDeleteMutex);
                if (!g_texturesToDelete.empty()) {
                    glDeleteTextures((GLsizei)g_texturesToDelete.size(), g_texturesToDelete.data());
                    g_texturesToDelete.clear();
                }
                g_hasTexturesToDelete.store(false, std::memory_order_release);
            }
        }

        // Note: Image processing is now done in render_thread

        if (g_pendingImageLoad) {
            PROFILE_SCOPE_CAT("Pending Image Load", "SwapBuffers");
            LoadAllImages();
            g_allImagesLoaded = true;
            g_pendingImageLoad = false;
        }

        int current_gameW = modeToRenderCopy.width;
        int current_gameH = modeToRenderCopy.height;

        g_obsCaptureReady.store(false);


        bool hideAnimOnScreen = frameCfg.hideAnimationsInGame && IsModeTransitionActive();

        {
            PROFILE_SCOPE_CAT("Normal Mode Handling", "Rendering");

            if (needsDualRendering) {
                // Submit animated frame to render thread for OBS capture using helper function
                {
                    PROFILE_SCOPE_CAT("Submit OBS Frame", "OBS");

                    // Build lightweight context struct (no lock-free reads needed here - values already captured)
                    ObsFrameSubmission submission;
                    submission.context.fullW = fullW;
                    submission.context.fullH = fullH;
                    submission.context.gameW = current_gameW;
                    submission.context.gameH = current_gameH;
                    submission.context.gameTextureId = g_cachedGameTextureId.load();
                    submission.context.modeId = modeToRenderCopy.id;
                    submission.context.relativeStretching = modeToRenderCopy.relativeStretching;
                    submission.context.bgR = modeToRenderCopy.background.color.r;
                    submission.context.bgG = modeToRenderCopy.background.color.g;
                    submission.context.bgB = modeToRenderCopy.background.color.b;
                    submission.context.shouldRenderGui = shouldRenderGui;
                    submission.context.showPerformanceOverlay = showPerformanceOverlay;
                    submission.context.showProfiler = showProfiler;
                    submission.context.isEyeZoom = isEyeZoom;
                    submission.context.isTransitioningFromEyeZoom = isTransitioningFromEyeZoom;
                    submission.context.eyeZoomAnimatedViewportX = eyeZoomAnimatedViewportX;
                    submission.context.eyeZoomSnapshotTexture = GetEyeZoomSnapshotTexture();
                    submission.context.eyeZoomSnapshotWidth = GetEyeZoomSnapshotWidth();
                    submission.context.eyeZoomSnapshotHeight = GetEyeZoomSnapshotHeight();
                    submission.context.showTextureGrid = frameCfg.debug.showTextureGrid;
                    submission.context.isWindowed = isWindowedPresentation;
                    submission.context.isRawWindowedMode = false;
                    submission.context.windowW = windowWidth;
                    submission.context.windowH = windowHeight;
                    submission.context.welcomeToastIsFullscreen = EqualsIgnoreCase(modeToRenderCopy.id, "Fullscreen");
                    submission.context.showWelcomeToast = false;
                    submission.isDualRenderingPath = hideAnimOnScreen;

                    // Create fence and flush - these MUST be on GL thread
                    submission.gameTextureFence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
                    glFlush();

                    // Submit lightweight context - render thread will call BuildObsFrameRequest
                    SubmitObsFrameContext(submission);
                }

                PROFILE_SCOPE_CAT("Render for Screen", "Rendering");
                RenderMode(&modeToRenderCopy, s, current_gameW, current_gameH, hideAnimOnScreen, false);

            } else {
                RenderMode(&modeToRenderCopy, s, current_gameW, current_gameH, hideAnimOnScreen, false);

            }
        }

        // All ImGui rendering is handled by render thread (via FrameRenderRequest ImGui state fields)
        // Screenshot handling stays on main thread since it needs direct backbuffer access
        if (g_screenshotRequested.exchange(false)) {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, s.read_fb);
            ScreenshotToClipboard(fullW, fullH);
        }

        // Render fake cursor overlay if enabled (MUST be after RestoreGLState)
        {
            bool fakeCursorEnabled = frameCfg.debug.fakeCursor;
            if (fakeCursorEnabled) {
                PROFILE_SCOPE_CAT("Fake Cursor Rendering", "Rendering");
                if (IsCursorVisible()) { RenderFakeCursor(hwnd, windowWidth, windowHeight); }
            }
        }

        {
            PROFILE_SCOPE_CAT("OpenGL State Restore", "SwapBuffers");
            RestoreGLState(s);
        }

        Profiler::GetInstance().EndFrame();

        // Update last frame mode ID using lock-free double-buffer
        // We're the only writer on this thread, so no lock needed - just atomic swap
        {
            int nextIndex = 1 - g_lastFrameModeIdIndex.load(std::memory_order_relaxed);
            g_lastFrameModeIdBuffers[nextIndex] = desiredModeId;
            g_lastFrameModeIdIndex.store(nextIndex, std::memory_order_release);
            g_lastFrameModeId = desiredModeId; // Keep legacy variable in sync (no lock needed - single writer)
        }

        g_isTransitioningMode = false;

        int targetFPS = 0;
        { targetFPS = frameCfg.fpsLimit; }

        if (targetFPS > 0 && g_highResTimer) {
            PROFILE_SCOPE_CAT("FPS Limit Sleep", "Timing");

            const double targetFrameTimeUs = 1000000.0 / targetFPS;
            const bool isHighFPS = targetFPS > 500;

            std::lock_guard<std::mutex> lock(g_fpsLimitMutex);

            auto targetTime = g_lastFrameEndTime + std::chrono::microseconds(static_cast<long long>(targetFrameTimeUs));
            auto now = std::chrono::high_resolution_clock::now();

            if (now < targetTime) {
                auto timeToWaitUs = std::chrono::duration_cast<std::chrono::microseconds>(targetTime - now).count();

                if (isHighFPS) {
                    if (timeToWaitUs > 1000) {
                        LARGE_INTEGER dueTime;
                        dueTime.QuadPart = -static_cast<LONGLONG>(timeToWaitUs);

                        if (SetWaitableTimer(g_highResTimer, &dueTime, 0, NULL, NULL, FALSE)) {
                            WaitForSingleObject(g_highResTimer, 1000);
                        }
                    }
                } else {
                    if (timeToWaitUs > 10) {
                        LARGE_INTEGER dueTime;
                        dueTime.QuadPart = -static_cast<LONGLONG>(timeToWaitUs * 10LL);

                        if (SetWaitableTimer(g_highResTimer, &dueTime, 0, NULL, NULL, FALSE)) { WaitForSingleObject(g_highResTimer, 1000); }
                    }
                }

                g_lastFrameEndTime = targetTime;
            } else {
                g_lastFrameEndTime = now;
            }
        }

        if (IsModeTransitionActive()) {
            PROFILE_SCOPE_CAT("Mode Transition Animation", "SwapBuffers");
            UpdateModeTransition();
        }

        if (frameCfg.debug.delayRenderingUntilFinished) { glFinish(); }

        // Optionally wait for the async overlay blit fence to complete before SwapBuffers
        if (frameCfg.debug.delayRenderingUntilBlitted) { WaitForOverlayBlitFence(); }

        auto swapStartTime = std::chrono::high_resolution_clock::now();
        BOOL result = next(hDc);

        g_safeToCapture.store(false, std::memory_order_release);

        auto swapEndTime = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double, std::milli> swapDuration = swapEndTime - swapStartTime;
        g_originalFrameTimeMs = swapDuration.count();

        std::chrono::duration<double, std::milli> fp_ms = swapStartTime - startTime;
        g_lastFrameTimeMs = fp_ms.count();

        // Update last frame mode ID for next frame's viewport calculations (lock-free)
        {
            int nextIndex = 1 - g_lastFrameModeIdIndex.load(std::memory_order_relaxed);
            g_lastFrameModeIdBuffers[nextIndex] = desiredModeId;
            g_lastFrameModeIdIndex.store(nextIndex, std::memory_order_release);
            g_lastFrameModeId = desiredModeId;
        }

        return result;
    } catch (const SE_Exception& e) {
        LogException("hkwglSwapBuffers (SEH)", e.getCode(), e.getInfo());
        return next(hDc);
    } catch (const std::exception& e) {
        LogException("hkwglSwapBuffers", e);
        return next(hDc);
    } catch (...) {
        Log("FATAL UNKNOWN EXCEPTION in hkwglSwapBuffers!");
        return next(hDc);
    }
}

BOOL WINAPI hkwglSwapBuffers(HDC hDc) { return SwapBuffersHook_Impl(owglSwapBuffers, hDc); }

BOOL WINAPI hkwglSwapBuffers_ThirdParty(HDC hDc) {
    WGLSWAPBUFFERS next = owglSwapBuffers;
    if (g_config.hookChainingNextTarget == HookChainingNextTarget::LatestHook) {
        next = g_owglSwapBuffersThirdParty ? g_owglSwapBuffersThirdParty : owglSwapBuffers;
    }
    return SwapBuffersHook_Impl(next, hDc);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)DllMain,
                           &g_hModule);

        InstallGlobalExceptionHandlers();

        LogCategory("init", "========================================");
        LogCategory("init", "=== Toolscreen INITIALIZATION START ===");
        LogCategory("init", "========================================");
        PrintVersionToStdout();

        // Create high-resolution waitable timer for FPS limiting (Windows 10 1803+)
        g_highResTimer = CreateWaitableTimerExW(NULL,
                                                NULL,
                                                CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                                TIMER_ALL_ACCESS
        );
        if (g_highResTimer) {
            LogCategory("init", "High-resolution waitable timer created successfully for FPS limiting.");
        } else {
            Log("Warning: Failed to create high-resolution waitable timer. FPS limiting may be less precise.");
        }

        g_toolscreenPath = GetToolscreenPath();
        if (!g_toolscreenPath.empty()) {
            std::wstring logsDir = g_toolscreenPath + L"\\logs";
            CreateDirectoryW(logsDir.c_str(), NULL);

            std::wstring latestLogPath = logsDir + L"\\latest.log";

            if (GetFileAttributesW(latestLogPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
                HANDLE hFile =
                    CreateFileW(latestLogPath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
                if (hFile != INVALID_HANDLE_VALUE) {
                    FILETIME lastWriteTime;
                    if (GetFileTime(hFile, NULL, NULL, &lastWriteTime)) {
                        FILETIME localFileTime;
                        FileTimeToLocalFileTime(&lastWriteTime, &localFileTime);
                        SYSTEMTIME st;
                        FileTimeToSystemTime(&localFileTime, &st);

                        WCHAR timestamp[32];
                        swprintf_s(timestamp, L"%04d%02d%02d_%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

                        std::wstring archivedLogPath = logsDir + L"\\" + timestamp + L".log";

                        CloseHandle(hFile);

                        if (GetFileAttributesW(archivedLogPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
                            for (int counter = 1; counter < 100; counter++) {
                                std::wstring altPath = logsDir + L"\\" + timestamp + L"_" + std::to_wstring(counter) + L".log";
                                if (GetFileAttributesW(altPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
                                    archivedLogPath = altPath;
                                    break;
                                }
                            }
                        }

                        if (!MoveFileW(latestLogPath.c_str(), archivedLogPath.c_str())) {
                            Log("WARNING: Could not rename old log to " + WideToUtf8(archivedLogPath) +
                                ", error code: " + std::to_string(GetLastError()));
                        } else {
                            // Compress the archived log to .gz on a background thread
                            // so we don't block DLL initialization
                            std::wstring archiveSrc = archivedLogPath;
                            std::thread([archiveSrc]() {
                                std::wstring gzPath = archiveSrc + L".gz";
                                if (CompressFileToGzip(archiveSrc, gzPath)) {
                                    DeleteFileW(archiveSrc.c_str());
                                }
                            }).detach();
                        }
                    } else {
                        CloseHandle(hFile);
                    }
                }
            }

            {
                std::lock_guard<std::mutex> lock(g_logFileMutex);
                // Open via std::filesystem::path so wide Win32 APIs are used.
                logFile.open(std::filesystem::path(latestLogPath), std::ios_base::out | std::ios_base::trunc);
            }

            // Start async logging thread now that log file is open
            StartLogThread();

            g_modeFilePath = g_toolscreenPath + L"\\mode.txt";
        }
        LogCategory("init", "--- DLL instance attached ---");
        LogVersionInfo();
        if (g_toolscreenPath.empty()) { Log("FATAL: Could not get toolscreen directory."); }
        
        StartSupportersFetch();

        g_gameVersion = GetGameVersionFromCommandLine();
        GameVersion minVersion(1, 16, 1);
        GameVersion maxVersion(1, 18, 2);

        if (g_gameVersion.valid) {
            bool inRange = IsVersionInRange(g_gameVersion, minVersion, maxVersion);

            std::ostringstream oss;
            oss << "Game version " << g_gameVersion.major << "." << g_gameVersion.minor << "." << g_gameVersion.patch;
            if (inRange) {
                oss << " is in supported range [1.16.1 - 1.18.2].";
            } else {
                oss << " is outside supported range [1.16.1 - 1.18.2].";
            }
            LogCategory("init", oss.str());
        } else {
            LogCategory("init", "No game version detected from command line.");
        }

        LoadConfig();

        WCHAR dir[MAX_PATH];
        if (GetCurrentDirectoryW(MAX_PATH, dir) > 0) {
            g_stateFilePath = std::wstring(dir) + L"\\wpstateout.txt";
            LogCategory("init", "State file path set to: " + WideToUtf8(g_stateFilePath));

            DWORD stateFileAttrs = GetFileAttributesW(g_stateFilePath.c_str());
            bool stateOutputAvailable = (stateFileAttrs != INVALID_FILE_ATTRIBUTES) && !(stateFileAttrs & FILE_ATTRIBUTE_DIRECTORY);
            g_isStateOutputAvailable.store(stateOutputAvailable, std::memory_order_release);
            if (!stateOutputAvailable) {
                LogCategory(
                    "init",
                    "WARNING: wpstateout.txt not found. Game-state hotkey restrictions will not apply until State Output is installed.");
            }
        } else {
            Log("FATAL: Could not get current directory for state file path.");
        }

        // Use std::thread instead of CreateThread to ensure proper CRT per-thread
        // initialization (locale facets, errno, etc.). CreateThread skips CRT init which
        g_monitorThread = std::thread([]() { FileMonitorThread(nullptr); });
        g_imageMonitorThread = std::thread([]() { ImageMonitorThread(nullptr); });

        StartWindowCaptureThread();

        if (MH_Initialize() != MH_OK) {
            Log("ERROR: MH_Initialize() failed!");
            return TRUE;
        }

        LogCategory("init", "Setting up hooks...");

        HMODULE hOpenGL32 = GetModuleHandle(L"opengl32.dll");
        HMODULE hUser32 = GetModuleHandle(L"user32.dll");
        HMODULE hGlfw = GetModuleHandle(L"glfw.dll");

        if (!hOpenGL32) {
            Log("ERROR: GetModuleHandle(opengl32.dll) returned NULL");
            return TRUE;
        }
        if (!hUser32) {
            Log("ERROR: GetModuleHandle(user32.dll) returned NULL");
            return TRUE;
        }

#define HOOK(mod, name) CreateHookOrDie(GetProcAddress(mod, #name), &hk##name, &o##name, #name)
        HOOK(hOpenGL32, wglSwapBuffers);
        if (IsVersionInRange(g_gameVersion, GameVersion(1, 0, 0), GameVersion(1, 21, 0))) {
            if (HOOK(hOpenGL32, glViewport)) {
                g_glViewportHookCount.fetch_add(1);
                LogCategory("init", "Initial glViewport hook created via opengl32.dll");
            }
        }
        HOOK(hUser32, SetCursorPos);
        HOOK(hUser32, ClipCursor);
        HOOK(hUser32, SetCursor);
        HOOK(hUser32, GetRawInputData);
        if (hGlfw) {
            HOOK(hGlfw, glfwSetInputMode);
        } else {
            LogCategory("init", "WARNING: glfw.dll not loaded; skipping glfwSetInputMode hook");
        }
#undef HOOK

        LPVOID pGlBlitNamedFramebuffer = GetProcAddress(hOpenGL32, "glBlitNamedFramebuffer");
        if (pGlBlitNamedFramebuffer != NULL) {
            CreateHookOrDie(pGlBlitNamedFramebuffer, &hkglBlitNamedFramebuffer, &oglBlitNamedFramebuffer, "glBlitNamedFramebuffer");
        } else {
            LogCategory("init",
                        "WARNING: glBlitNamedFramebuffer not found in opengl32.dll - will attempt to hook via GLEW after context init");
        }

        if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
            Log("ERROR: MH_EnableHook(MH_ALL_HOOKS) failed!");
            return TRUE;
        }

        LogCategory("init", "Hooks enabled.");

        // This thread periodically detects those detours (prolog or IAT) and chains behind them.
        g_stopHookCompat.store(false, std::memory_order_release);
        g_hookCompatThread = std::thread([]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            while (!g_stopHookCompat.load(std::memory_order_acquire) && !g_isShuttingDown.load(std::memory_order_acquire)) {
                HookChain::RefreshAllThirdPartyHookChains();

                for (int i = 0; i < 20; i++) {
                    if (g_stopHookCompat.load(std::memory_order_acquire) || g_isShuttingDown.load(std::memory_order_acquire)) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }
        });

        // Save the original Windows mouse speed so we can restore it on exit
        SaveOriginalWindowsMouseSpeed();

        SaveOriginalKeyRepeatSettings();

        ApplyKeyRepeatSettings();

    } else if (ul_reason_for_call == DLL_PROCESS_DETACH) {
        // We should do MINIMAL cleanup here. Windows will automatically clean up:
        // - GPU resources (driver handles cleanup)
        // - Thread handles
        // 1. Other threads may still be running

        g_isShuttingDown = true;
        Log("DLL Detached. Performing minimal cleanup...");

        if (g_highResTimer) {
            CloseHandle(g_highResTimer);
            g_highResTimer = NULL;
        }

        // ONLY save config and stop our own threads
        // Do NOT touch hooks, GPU resources, or game state

        // Restore original Windows mouse speed before exiting
        RestoreWindowsMouseSpeed();

        RestoreKeyRepeatSettings();

        SaveConfigImmediate();
        Log("Config saved.");

        // Stop monitoring threads
        g_stopMonitoring = true;
        if (g_monitorThread.joinable()) { g_monitorThread.join(); }

        g_stopImageMonitoring = true;
        if (g_imageMonitorThread.joinable()) { g_imageMonitorThread.join(); }

        // Stop hook compatibility monitor thread
        g_stopHookCompat.store(true, std::memory_order_release);
        if (g_hookCompatThread.joinable()) { g_hookCompatThread.join(); }

        // Stop background threads
        StopWindowCaptureThread();

        CleanupSharedContexts();

        Log("Background threads stopped.");

        // Clean up CPU-allocated memory that won't be freed by Windows
        {
            std::lock_guard<std::mutex> lock(g_decodedImagesMutex);
            for (auto& decodedImg : g_decodedImagesQueue) {
                if (decodedImg.data) { stbi_image_free(decodedImg.data); }
            }
            g_decodedImagesQueue.clear();
        }

        // DO NOT:
        // - Delete GPU resources (Windows/driver handles this)

        Log("DLL cleanup complete (minimal cleanup strategy).");

        // Stop async logging thread and flush all pending logs
        StopLogThread();
        FlushLogs();

        {
            std::lock_guard<std::mutex> lock(g_logFileMutex);
            if (logFile.is_open()) {
                logFile.flush();
                logFile.close();
            }
        }
    }
    return TRUE;
}
