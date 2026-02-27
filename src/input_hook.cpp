#include "input_hook.h"

#include "fake_cursor.h"
#include "gui.h"
#include "imgui_cache.h"
#include "logic_thread.h"
#include "profiler.h"
#include "render.h"
#include "utils.h"
#include "version.h"
#include "window_overlay.h"

#include "imgui_impl_win32.h"

#include "imgui_input_queue.h"

#include <chrono>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <windowsx.h>

extern std::atomic<bool> g_showGui;
extern std::atomic<bool> g_guiNeedsRecenter;
extern std::atomic<bool> g_wasCursorVisible;
extern std::atomic<bool> g_isShuttingDown;
extern std::atomic<HWND> g_subclassedHwnd;
extern WNDPROC g_originalWndProc;
extern std::atomic<bool> g_configLoadFailed;
extern std::atomic<int> g_wmMouseMoveCount;
extern GameVersion g_gameVersion;
extern Config g_config;

extern std::string g_currentModeId;
extern std::mutex g_modeIdMutex;
extern std::string g_gameStateBuffers[2];
extern std::atomic<int> g_currentGameStateIndex;
extern std::string g_currentlyEditingMirror;

extern std::atomic<bool> g_imageDragMode;
extern std::atomic<bool> g_windowOverlayDragMode;
extern std::atomic<HCURSOR> g_specialCursorHandle;
// g_glInitialized is declared in render.h as atomic bool
extern std::atomic<bool> g_gameWindowActive;

extern std::unordered_map<std::string, std::chrono::steady_clock::time_point> g_hotkeyTimestamps;
extern std::mutex g_hotkeyTimestampsMutex;
extern std::unordered_set<DWORD> g_hotkeyMainKeys;
extern std::mutex g_hotkeyMainKeysMutex;
extern std::set<std::string> g_triggerOnReleasePending;
extern std::set<std::string> g_triggerOnReleaseInvalidated;
extern std::mutex g_triggerOnReleaseMutex;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
static bool s_forcedShowCursor = false;

static void EnsureSystemCursorVisible() {
    if (g_gameVersion < GameVersion(1, 13, 0)) { return; }

    CURSORINFO ci{ sizeof(CURSORINFO) };
    if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING)) { return; }
    ShowCursor(TRUE);
}

static void EnsureSystemCursorHidden() {
    if (g_gameVersion < GameVersion(1, 13, 0)) { return; }

    CURSORINFO ci{ sizeof(CURSORINFO) };
    if (GetCursorInfo(&ci) && !(ci.flags & CURSOR_SHOWING)) { return; }
    ShowCursor(FALSE);
}

static DWORD NormalizeModifierVkFromKeyMessage(DWORD rawVk, LPARAM lParam) {
    DWORD vk = rawVk;

    const UINT scanCode = static_cast<UINT>((lParam >> 16) & 0xFF);
    const bool isExtended = (lParam & (1LL << 24)) != 0;

    if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT) {
        if (scanCode != 0) {
            DWORD mapped = static_cast<DWORD>(::MapVirtualKeyW(scanCode, MAPVK_VSC_TO_VK_EX));
            if (mapped == VK_LSHIFT || mapped == VK_RSHIFT) {
                vk = mapped;
            }
        }
        return vk;
    }

    if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL) {
        return isExtended ? VK_RCONTROL : VK_LCONTROL;
    }
    if (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU) {
        return isExtended ? VK_RMENU : VK_LMENU;
    }

    return vk;
}

static DWORD NormalizeModifierVkFromConfig(DWORD vk, UINT scanCodeWithFlags = 0) {
    const UINT scanLow = scanCodeWithFlags & 0xFF;
    const bool isExtended = (scanCodeWithFlags & 0xFF00) != 0;

    switch (vk) {
    case VK_SHIFT:
        if (scanLow == 0x36) return VK_RSHIFT;
        return VK_LSHIFT;
    case VK_CONTROL:
        return isExtended ? VK_RCONTROL : VK_LCONTROL;
    case VK_MENU:
        return isExtended ? VK_RMENU : VK_LMENU;
    default:
        return vk;
    }
}

InputHandlerResult HandleMouseMoveViewportOffset(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM& lParam) {
    if (uMsg != WM_MOUSEMOVE) { return { false, 0 }; }
    PROFILE_SCOPE("HandleMouseMoveViewportOffset");
    // Legacy compatibility path intentionally disabled.
    // Mouse translation is handled centrally in HandleMouseCoordinateTranslationPhase.
    (void)hWnd;
    (void)uMsg;
    (void)wParam;
    (void)lParam;
    return { false, 0 };
}

InputHandlerResult HandleShutdownCheck(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    PROFILE_SCOPE("HandleShutdownCheck");

    if (g_isShuttingDown.load() && g_originalWndProc) { return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) }; }
    return { false, 0 };
}

InputHandlerResult HandleWindowValidation(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    PROFILE_SCOPE("HandleWindowValidation");

    if (g_subclassedHwnd.load() != hWnd) {
        Log("WARNING: SubclassedWndProc called for unexpected window " + std::to_string(reinterpret_cast<uintptr_t>(hWnd)) + " (expected " +
            std::to_string(reinterpret_cast<uintptr_t>(g_subclassedHwnd.load())) + ")");
        if (g_originalWndProc) { return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) }; }
        return { true, DefWindowProc(hWnd, uMsg, wParam, lParam) };
    }
    return { false, 0 };
}

static UINT GetToolscreenIsInstalledMessageId() {
    static const UINT s_msg = RegisterWindowMessageA("Toolscreen_IsInstalled");
    return s_msg;
}

static UINT GetToolscreenGetVersionMessageId() {
    static const UINT s_msg = RegisterWindowMessageA("Toolscreen_GetVersion");
    return s_msg;
}

static LRESULT EncodeToolscreenVersionNumber() {
    // 0x00MMmmpp (major/minor/patch in 8-bit fields)
    return static_cast<LRESULT>(((TOOLSCREEN_VERSION_MAJOR & 0xFF) << 16) |
                                ((TOOLSCREEN_VERSION_MINOR & 0xFF) << 8) |
                                (TOOLSCREEN_VERSION_PATCH & 0xFF));
}

InputHandlerResult HandleToolscreenQueryMessages(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    const UINT isInstalledMsg = GetToolscreenIsInstalledMessageId();
    const UINT getVersionMsg = GetToolscreenGetVersionMessageId();
    if (uMsg != isInstalledMsg && uMsg != getVersionMsg) { return { false, 0 }; }
    PROFILE_SCOPE("HandleToolscreenQueryMessages");
    (void)hWnd;
    (void)wParam;
    (void)lParam;

    if (uMsg == isInstalledMsg) {
        return { true, 1 };
    }

    if (uMsg == getVersionMsg) {
        return { true, EncodeToolscreenVersionNumber() };
    }

    return { false, 0 };
}

InputHandlerResult HandleNonFullscreenCheck(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    PROFILE_SCOPE("HandleNonFullscreenCheck");

    // Windowed mode is supported; do not bypass the hook pipeline.
    return { false, 0 };
}

void HandleCharLogging(UINT uMsg, WPARAM wParam, LPARAM lParam) {
    auto cfgSnap = GetConfigSnapshot();
    if (uMsg == WM_CHAR && cfgSnap && cfgSnap->debug.showHotkeyDebug) {
        Log("WM_CHAR: " + std::to_string(wParam) + " " + std::to_string(lParam));
    }
}

InputHandlerResult HandleWindowPosChanged(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg != WM_WINDOWPOSCHANGED) { return { false, 0 }; }
    PROFILE_SCOPE("HandleWindowPosChanged");

    auto forwardToOriginal = [&]() -> InputHandlerResult {
        if (g_originalWndProc) { return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) }; }
        return { true, DefWindowProc(hWnd, uMsg, wParam, lParam) };
    };

    if (lParam == 0) {
        Log("[RESIZE] Ignoring WM_WINDOWPOSCHANGED with null WINDOWPOS");
        return forwardToOriginal();
    }

    WINDOWPOS* pos = reinterpret_cast<WINDOWPOS*>(lParam);
    const int flags = pos->flags;
    const bool sizeChanged = (flags & SWP_NOSIZE) == 0;
    const bool moveChanged = (flags & SWP_NOMOVE) == 0;
    if (!sizeChanged && !moveChanged) { return forwardToOriginal(); }

    const int currentX = pos->x;
    const int currentY = pos->y;
    const int currentWidth = pos->cx;
    const int currentHeight = pos->cy;

    if (currentX == -32000 && currentY == -32000) {
        Log("[RESIZE] Ignoring WM_WINDOWPOSCHANGED with minimized coordinates");
        return forwardToOriginal();
    }

    Log("[RESIZE] External resize detected to " + std::to_string(currentWidth) + "x" + std::to_string(currentHeight) + " at (" +
        std::to_string(currentX) + "," + std::to_string(currentY) + "), flags=" + std::to_string(flags));

    // In windowed mode, keep user-driven position/size and just refresh dependent caches.
    if (!IsFullscreen()) {
        if (sizeChanged) {
            RequestScreenMetricsRecalculation();
        } else {
            InvalidateCachedScreenMetrics();
        }
        if (sizeChanged) { g_cachedGameTextureId.store(UINT_MAX); }
        return forwardToOriginal();
    }

    RECT targetRect{};
    if (!GetMonitorRectForWindow(hWnd, targetRect)) {
        if (!GetWindowRect(hWnd, &targetRect)) { return forwardToOriginal(); }
    }
    const int targetW = (targetRect.right - targetRect.left);
    const int targetH = (targetRect.bottom - targetRect.top);
    if (targetW <= 0 || targetH <= 0) { return forwardToOriginal(); }
    SetWindowPos(hWnd, HWND_NOTOPMOST, targetRect.left, targetRect.top, targetW, targetH, SWP_NOACTIVATE | SWP_NOOWNERZORDER);

    g_cachedGameTextureId.store(UINT_MAX);

    return forwardToOriginal();
}

InputHandlerResult HandleAltF4(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg != WM_SYSKEYDOWN) { return { false, 0 }; }
    PROFILE_SCOPE("HandleAltF4");

    if (wParam == VK_F4) { return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) }; }
    return { false, 0 };
}

InputHandlerResult HandleConfigLoadFailure(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    PROFILE_SCOPE("HandleConfigLoadFailure");

    if (!g_configLoadFailed.load()) { return { false, 0 }; }

    if (ImGui_ImplWin32_WndProcHandler(hWnd, uMsg, wParam, lParam)) { return { true, true }; }

    switch (uMsg) {
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MBUTTONDBLCLK:
    case WM_MOUSEWHEEL:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_XBUTTONDBLCLK:
    case WM_INPUT:
        return { true, 1 };
    default:
        break;
    }
    return { false, 0 };
}

InputHandlerResult HandleSetCursor(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, const std::string& gameState) {
    if (uMsg != WM_SETCURSOR) { return { false, 0 }; }
    PROFILE_SCOPE("HandleSetCursor");

    if (g_showGui.load() && s_forcedShowCursor && g_gameVersion >= GameVersion(1, 13, 0)) {
        EnsureSystemCursorVisible();
        static HCURSOR s_arrowCursor = LoadCursorW(NULL, IDC_ARROW);
        SetCursor(s_arrowCursor);
        return { true, true };
    }

    if (!IsCursorVisible() && !g_showGui.load()) {
        SetCursor(NULL);
        return { true, true };
    }

    const CursorTextures::CursorData* cursorData = CursorTextures::GetSelectedCursor(gameState, 64);
    if (cursorData && cursorData->hCursor) {
        SetCursor(cursorData->hCursor);
        return { true, true };
    }
    return { false, 0 };
}

InputHandlerResult HandleDestroy(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg != WM_DESTROY) { return { false, 0 }; }
    PROFILE_SCOPE("HandleDestroy");

    extern GameVersion g_gameVersion;
    if (g_gameVersion >= GameVersion(1, 13, 0)) { g_isShuttingDown = true; }
    return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) };
}

InputHandlerResult HandleImGuiInput(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    PROFILE_SCOPE("HandleImGuiInput");

    if (!g_showGui.load()) { return { false, 0 }; }

    // Never call ImGui from this thread.
    // Instead, enqueue the message for the render thread (which owns the ImGui context).
    ImGuiInputQueue_EnqueueWin32Message(hWnd, uMsg, wParam, lParam);
    return { false, 0 };
}

InputHandlerResult HandleGuiToggle(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN:
        break;
    default:
        return { false, 0 };
    }
    PROFILE_SCOPE("HandleGuiToggle");

    DWORD vkCode = 0;
    bool isEscape = false;
    switch (uMsg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        vkCode = static_cast<DWORD>(wParam);
        isEscape = (wParam == VK_ESCAPE);
        vkCode = NormalizeModifierVkFromKeyMessage(vkCode, lParam);
        break;
    }
    case WM_LBUTTONDOWN:
        vkCode = VK_LBUTTON;
        break;
    case WM_RBUTTONDOWN:
        vkCode = VK_RBUTTON;
        break;
    case WM_MBUTTONDOWN:
        vkCode = VK_MBUTTON;
        break;
    case WM_XBUTTONDOWN: {
        WORD xButton = GET_XBUTTON_WPARAM(wParam);
        vkCode = (xButton == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;
        break;
    }
    default:
        return { false, 0 };
    }

    if (!isEscape && !CheckHotkeyMatch(g_config.guiHotkey, vkCode)) { return { false, 0 }; }

    if (g_showGui.load(std::memory_order_acquire) && !isEscape) {
        switch (uMsg) {
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_XBUTTONDOWN:
            return { false, 0 };
        default:
            break;
        }
    }

    bool allow_toggle = true;
    if (isEscape && !g_showGui.load()) { allow_toggle = false; }

    if (!allow_toggle) { return { false, 0 }; }

    // Lock-free debouncing using atomic timestamp
    auto now = std::chrono::steady_clock::now();
    int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    int64_t lastMs = g_lastGuiToggleTimeMs.load(std::memory_order_relaxed);
    if (nowMs - lastMs < 200) {
        return { true, 1 };
    }
    g_lastGuiToggleTimeMs.store(nowMs, std::memory_order_relaxed);

    if (!g_glInitialized.load(std::memory_order_acquire)) {
        Log("GUI toggle ignored - OpenGL not initialized yet");
        return { true, 1 };
    }

    bool is_closing = g_showGui.load();

    if (isEscape && g_imguiAnyItemActive.load(std::memory_order_acquire)) { is_closing = false; }
    if (isEscape && IsHotkeyBindingActive()) { is_closing = false; }
    if (isEscape && IsRebindBindingActive()) { is_closing = false; }

    if (is_closing) {
        g_showGui = false;
        InvalidateImGuiCache();
        if (s_forcedShowCursor) {
            EnsureSystemCursorHidden();
            s_forcedShowCursor = false;
        }

        // Flush any queued ImGui input and release any mouse capture we may have taken.
        ImGuiInputQueue_Clear();
        ImGuiInputQueue_ResetMouseCapture(hWnd);

        if (!g_wasCursorVisible.load()) {
            RECT clipRect{};
            if (GetWindowClientRectInScreen(hWnd, clipRect)) {
                ClipCursor(&clipRect);
            } else {
                ClipCursor(NULL);
            }
            SetCursor(NULL);

            if (g_gameVersion < GameVersion(1, 13, 0)) {
                HCURSOR airCursor = g_specialCursorHandle.load();
                if (airCursor) SetCursor(airCursor);
            }
        }
        g_currentlyEditingMirror = "";

        g_imageDragMode.store(false);
        g_windowOverlayDragMode.store(false);

        extern std::string s_hoveredImageName;
        extern std::string s_draggedImageName;
        extern bool s_isDragging;
        s_hoveredImageName = "";
        s_draggedImageName = "";
        s_isDragging = false;

        extern std::string s_hoveredWindowOverlayName;
        extern std::string s_draggedWindowOverlayName;
        extern bool s_isWindowOverlayDragging;
        s_hoveredWindowOverlayName = "";
        s_draggedWindowOverlayName = "";
        s_isWindowOverlayDragging = false;
    } else if (!isEscape) {
        g_showGui = true;
        InvalidateImGuiCache();
        const bool wasCursorVisible = IsCursorVisible();
        g_wasCursorVisible = wasCursorVisible;
        g_guiNeedsRecenter = true;
        ClipCursor(NULL);
        if (!wasCursorVisible && g_gameVersion >= GameVersion(1, 13, 0)) {
            s_forcedShowCursor = true;
            EnsureSystemCursorVisible();
            static HCURSOR s_arrowCursor = LoadCursorW(NULL, IDC_ARROW);
            SetCursor(s_arrowCursor);
        }

        g_configurePromptDismissedThisSession.store(true, std::memory_order_release);

        if (!g_toolscreenPath.empty()) {
            std::wstring flagPath = g_toolscreenPath + L"\\has_opened";
            HANDLE hFile = CreateFileW(flagPath.c_str(), GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hFile != INVALID_HANDLE_VALUE) { CloseHandle(hFile); }
        }
    }
    return { true, 1 };
}

InputHandlerResult HandleBorderlessToggle(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN:
        break;
    default:
        return { false, 0 };
    }
    PROFILE_SCOPE("HandleBorderlessToggle");

    if (g_showGui.load(std::memory_order_acquire)) { return { false, 0 }; }

    if (g_config.borderlessHotkey.empty()) { return { false, 0 }; }

    if (IsHotkeyBindingActive() || IsRebindBindingActive()) { return { false, 0 }; }

    DWORD vkCode = 0;
    switch (uMsg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        vkCode = static_cast<DWORD>(wParam);
        vkCode = NormalizeModifierVkFromKeyMessage(vkCode, lParam);
        break;
    }
    case WM_LBUTTONDOWN:
        vkCode = VK_LBUTTON;
        break;
    case WM_RBUTTONDOWN:
        vkCode = VK_RBUTTON;
        break;
    case WM_MBUTTONDOWN:
        vkCode = VK_MBUTTON;
        break;
    case WM_XBUTTONDOWN: {
        WORD xButton = GET_XBUTTON_WPARAM(wParam);
        vkCode = (xButton == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;
        break;
    }
    default:
        return { false, 0 };
    }

    if (!CheckHotkeyMatch(g_config.borderlessHotkey, vkCode)) { return { false, 0 }; }

    static std::atomic<int64_t> s_lastToggleMs{ 0 };
    auto now = std::chrono::steady_clock::now();
    int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    int64_t lastMs = s_lastToggleMs.load(std::memory_order_relaxed);
    if (nowMs - lastMs < 250) { return { true, 1 }; }
    s_lastToggleMs.store(nowMs, std::memory_order_relaxed);

    ToggleBorderlessWindowedFullscreen(hWnd);
    return { true, 1 };
}

InputHandlerResult HandleImageOverlaysToggle(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN:
        break;
    default:
        return { false, 0 };
    }
    PROFILE_SCOPE("HandleImageOverlaysToggle");

    if (g_showGui.load(std::memory_order_acquire)) { return { false, 0 }; }

    if (g_config.imageOverlaysHotkey.empty()) { return { false, 0 }; }

    if (IsHotkeyBindingActive() || IsRebindBindingActive()) { return { false, 0 }; }

    DWORD vkCode = 0;
    switch (uMsg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        vkCode = static_cast<DWORD>(wParam);
        vkCode = NormalizeModifierVkFromKeyMessage(vkCode, lParam);
        break;
    }
    case WM_LBUTTONDOWN:
        vkCode = VK_LBUTTON;
        break;
    case WM_RBUTTONDOWN:
        vkCode = VK_RBUTTON;
        break;
    case WM_MBUTTONDOWN:
        vkCode = VK_MBUTTON;
        break;
    case WM_XBUTTONDOWN: {
        WORD xButton = GET_XBUTTON_WPARAM(wParam);
        vkCode = (xButton == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;
        break;
    }
    default:
        return { false, 0 };
    }

    if (!CheckHotkeyMatch(g_config.imageOverlaysHotkey, vkCode)) { return { false, 0 }; }

    static std::atomic<int64_t> s_lastToggleMs{ 0 };
    auto now = std::chrono::steady_clock::now();
    int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    int64_t lastMs = s_lastToggleMs.load(std::memory_order_relaxed);
    if (nowMs - lastMs < 250) { return { true, 1 }; }
    s_lastToggleMs.store(nowMs, std::memory_order_relaxed);

    bool newVisible = !g_imageOverlaysVisible.load(std::memory_order_acquire);
    g_imageOverlaysVisible.store(newVisible, std::memory_order_release);

    return { true, 1 };
}

InputHandlerResult HandleWindowOverlaysToggle(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN:
        break;
    default:
        return { false, 0 };
    }
    PROFILE_SCOPE("HandleWindowOverlaysToggle");

    if (g_showGui.load(std::memory_order_acquire)) { return { false, 0 }; }

    if (g_config.windowOverlaysHotkey.empty()) { return { false, 0 }; }

    if (IsHotkeyBindingActive() || IsRebindBindingActive()) { return { false, 0 }; }

    DWORD vkCode = 0;
    switch (uMsg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        vkCode = static_cast<DWORD>(wParam);
        vkCode = NormalizeModifierVkFromKeyMessage(vkCode, lParam);
        break;
    }
    case WM_LBUTTONDOWN:
        vkCode = VK_LBUTTON;
        break;
    case WM_RBUTTONDOWN:
        vkCode = VK_RBUTTON;
        break;
    case WM_MBUTTONDOWN:
        vkCode = VK_MBUTTON;
        break;
    case WM_XBUTTONDOWN: {
        WORD xButton = GET_XBUTTON_WPARAM(wParam);
        vkCode = (xButton == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;
        break;
    }
    default:
        return { false, 0 };
    }

    if (!CheckHotkeyMatch(g_config.windowOverlaysHotkey, vkCode)) { return { false, 0 }; }

    static std::atomic<int64_t> s_lastToggleMs{ 0 };
    auto now = std::chrono::steady_clock::now();
    int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    int64_t lastMs = s_lastToggleMs.load(std::memory_order_relaxed);
    if (nowMs - lastMs < 250) { return { true, 1 }; }
    s_lastToggleMs.store(nowMs, std::memory_order_relaxed);

    bool newVisible = !g_windowOverlaysVisible.load(std::memory_order_acquire);
    g_windowOverlaysVisible.store(newVisible, std::memory_order_release);

    if (!newVisible) {
        UnfocusWindowOverlay();
    }

    return { true, 1 };
}

InputHandlerResult HandleWindowOverlayKeyboard(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg != WM_KEYDOWN && uMsg != WM_KEYUP && uMsg != WM_SYSKEYDOWN && uMsg != WM_SYSKEYUP) { return { false, 0 }; }
    PROFILE_SCOPE("HandleWindowOverlayKeyboard");

    if (!g_windowOverlaysVisible.load(std::memory_order_acquire)) { return { false, 0 }; }

    bool isOverlayInteractionActive = IsWindowOverlayFocused();

    if (!isOverlayInteractionActive) { return { false, 0 }; }

    // Never query ImGui from this thread. Use state published by render thread.
    bool imguiWantsKeyboard = g_showGui.load() && g_imguiWantCaptureKeyboard.load(std::memory_order_acquire);

    if (!imguiWantsKeyboard) {
        if (ForwardKeyboardToWindowOverlay(uMsg, wParam, lParam)) { return { true, 1 }; }
    }
    return { false, 0 };
}

InputHandlerResult HandleWindowOverlayMouse(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg < WM_MOUSEFIRST || uMsg > WM_MOUSELAST) { return { false, 0 }; }
    PROFILE_SCOPE("HandleWindowOverlayMouse");

    if (!g_windowOverlaysVisible.load(std::memory_order_acquire)) { return { false, 0 }; }

    int mouseX, mouseY;

    // WM_MOUSEWHEEL and WM_MOUSEHWHEEL use SCREEN coordinates in lParam, not client coordinates
    if (uMsg == WM_MOUSEWHEEL || uMsg == WM_MOUSEHWHEEL) {
        POINT cursorPos;
        GetCursorPos(&cursorPos);
        if (ScreenToClient(hWnd, &cursorPos)) {
            mouseX = cursorPos.x;
            mouseY = cursorPos.y;
        } else {
            mouseX = cursorPos.x;
            mouseY = cursorPos.y;
        }
    } else {
        mouseX = GET_X_LPARAM(lParam);
        mouseY = GET_Y_LPARAM(lParam);
    }

    const int screenW = GetCachedScreenWidth();
    const int screenH = GetCachedScreenHeight();

    bool isOverlayInteractionActive = IsWindowOverlayFocused();

    if (isOverlayInteractionActive) {
        if (uMsg == WM_LBUTTONDOWN || uMsg == WM_RBUTTONDOWN || uMsg == WM_MBUTTONDOWN) {
            std::string focusedName = GetFocusedWindowOverlayName();
            std::string overlayAtPoint = GetWindowOverlayAtPoint(mouseX, mouseY, screenW, screenH);

            if (overlayAtPoint.empty() || overlayAtPoint != focusedName) {
                UnfocusWindowOverlay();
                if (!overlayAtPoint.empty()) {
                    FocusWindowOverlay(overlayAtPoint);
                    ForwardMouseToWindowOverlay(uMsg, mouseX, mouseY, wParam, screenW, screenH);
                    return { true, 1 };
                }
            } else {
                ForwardMouseToWindowOverlay(uMsg, mouseX, mouseY, wParam, screenW, screenH);
                return { true, 1 };
            }
        } else {
            ForwardMouseToWindowOverlay(uMsg, mouseX, mouseY, wParam, screenW, screenH);
            return { true, 1 };
        }
    } else if ((uMsg == WM_LBUTTONDOWN || uMsg == WM_RBUTTONDOWN || uMsg == WM_MBUTTONDOWN)) {
        const bool cursorVisible = IsCursorVisible();
        if (!(g_showGui.load() || cursorVisible)) {
            return { false, 0 };
        }
        std::string overlayAtPoint = GetWindowOverlayAtPoint(mouseX, mouseY, screenW, screenH);
        if (!overlayAtPoint.empty()) {
            FocusWindowOverlay(overlayAtPoint);
            ForwardMouseToWindowOverlay(uMsg, mouseX, mouseY, wParam, screenW, screenH);
            return { true, 1 };
        }
    }
    return { false, 0 };
}

InputHandlerResult HandleGuiInputBlocking(UINT uMsg) {
    switch (uMsg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_CHAR:
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MBUTTONDBLCLK:
    case WM_MOUSEWHEEL:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_XBUTTONDBLCLK:
    case WM_INPUT:
        break;
    default:
        return { false, 0 };
    }

    PROFILE_SCOPE("HandleGuiInputBlocking");

    if (!g_showGui.load()) { return { false, 0 }; }

    return { true, 1 };
}

void RestoreWindowsMouseSpeed();
void ApplyWindowsMouseSpeed();
void RestoreKeyRepeatSettings();
void ApplyKeyRepeatSettings();

InputHandlerResult HandleActivate(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, const std::string& currentModeId) {
    if (uMsg != WM_ACTIVATE) { return { false, 0 }; }
    PROFILE_SCOPE("HandleActivate");

    if (wParam == WA_INACTIVE) {
        ImGuiInputQueue_EnqueueFocus(false);

        if (auto cs = GetConfigSnapshot(); cs && cs->debug.showHotkeyDebug) Log("[WINDOW] Window became inactive.");
        extern std::atomic<bool> g_isGameFocused;
        g_isGameFocused.store(false);
        g_gameWindowActive.store(false);

        RestoreWindowsMouseSpeed();
        RestoreKeyRepeatSettings();
    } else {
        ImGuiInputQueue_EnqueueFocus(true);

        if (auto cs = GetConfigSnapshot(); cs && cs->debug.showHotkeyDebug) Log("[WINDOW] Window became active.");
        extern std::atomic<bool> g_isGameFocused;
        g_isGameFocused.store(true);
        g_gameWindowActive.store(true);

        ApplyWindowsMouseSpeed();
        ApplyKeyRepeatSettings();

        int wmWidth = 0;
        int wmHeight = 0;

        // In windowed/maximized states, preserve the real client size instead of forcing mode dimensions.
        if (!IsFullscreen()) {
            RECT clientRect{};
            if (GetClientRect(hWnd, &clientRect)) {
                const int clientW = clientRect.right - clientRect.left;
                const int clientH = clientRect.bottom - clientRect.top;
                if (clientW > 0 && clientH > 0) {
                    wmWidth = clientW;
                    wmHeight = clientH;
                }
            }
        }

        if (wmWidth <= 0 || wmHeight <= 0) {
            auto inputSnap = GetConfigSnapshot();
            const ModeConfig* mode = inputSnap ? GetModeFromSnapshot(*inputSnap, currentModeId) : nullptr;
            if (mode) {
                wmWidth = mode->width;
                wmHeight = mode->height;
            } else {
                Log("[WINDOW] WARNING: Current mode '" + currentModeId + "' not found in configuration!");
                return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) };
            }
        }

        PostMessage(hWnd, WM_SIZE, SIZE_RESTORED, MAKELPARAM(wmWidth, wmHeight));
    }
    return { false, 0 };
}

InputHandlerResult HandleWmSizeModeDimensions(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, const std::string& currentModeId) {
    if (uMsg != WM_SIZE) { return { false, 0 }; }
    PROFILE_SCOPE("HandleWmSizeModeDimensions");

    auto cfgSnap = GetConfigSnapshot();
    const ModeConfig* mode = cfgSnap ? GetModeFromSnapshot(*cfgSnap, currentModeId) : nullptr;
    if (!mode || mode->width <= 0 || mode->height <= 0) { return { false, 0 }; }

    const int msgW = LOWORD(lParam);
    const int msgH = HIWORD(lParam);
    if (msgW == mode->width && msgH == mode->height) { return { false, 0 }; }

    const LPARAM adjustedSize = MAKELPARAM(mode->width, mode->height);
    return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, adjustedSize) };
}

InputHandlerResult HandleHotkeys(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, const std::string& currentModeId,
                                 const std::string& gameState) {
    switch (uMsg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
        break;
    default:
        return { false, 0 };
    }
    PROFILE_SCOPE("HandleHotkeys");

    DWORD rawVkCode = 0;
    DWORD vkCode = 0;
    bool isKeyDown = false;

    if (uMsg == WM_KEYDOWN || uMsg == WM_SYSKEYDOWN) {
        rawVkCode = static_cast<DWORD>(wParam);
        isKeyDown = true;
    } else if (uMsg == WM_KEYUP || uMsg == WM_SYSKEYUP) {
        rawVkCode = static_cast<DWORD>(wParam);
        isKeyDown = false;
    } else if (uMsg == WM_XBUTTONDOWN) {
        WORD xButton = GET_XBUTTON_WPARAM(wParam);
        rawVkCode = (xButton == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;
        isKeyDown = true;
    } else if (uMsg == WM_XBUTTONUP) {
        WORD xButton = GET_XBUTTON_WPARAM(wParam);
        rawVkCode = (xButton == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;
        isKeyDown = false;
    } else if (uMsg == WM_LBUTTONDOWN) {
        rawVkCode = VK_LBUTTON;
        isKeyDown = true;
    } else if (uMsg == WM_LBUTTONUP) {
        rawVkCode = VK_LBUTTON;
        isKeyDown = false;
    } else if (uMsg == WM_RBUTTONDOWN) {
        rawVkCode = VK_RBUTTON;
        isKeyDown = true;
    } else if (uMsg == WM_RBUTTONUP) {
        rawVkCode = VK_RBUTTON;
        isKeyDown = false;
    } else if (uMsg == WM_MBUTTONDOWN) {
        rawVkCode = VK_MBUTTON;
        isKeyDown = true;
    } else if (uMsg == WM_MBUTTONUP) {
        rawVkCode = VK_MBUTTON;
        isKeyDown = false;
    } else {
        return { false, 0 };
    }

    // This mirrors imgui_impl_win32 behavior and enables reliable hotkeys + key rebinding.
    vkCode = rawVkCode;
    if (uMsg == WM_KEYDOWN || uMsg == WM_SYSKEYDOWN || uMsg == WM_KEYUP || uMsg == WM_SYSKEYUP) {
        vkCode = NormalizeModifierVkFromKeyMessage(rawVkCode, lParam);
        if (vkCode == 0) vkCode = rawVkCode;
    }

    // Even if resolution-change features are unsupported, we must not short-circuit the input pipeline.
    if (!IsResolutionChangeSupported(g_gameVersion)) { return { false, 0 }; }

    bool isHotkeyMainKey = false;
    {
        std::lock_guard<std::mutex> hotkeyLock(g_hotkeyMainKeysMutex);
        isHotkeyMainKey = (g_hotkeyMainKeys.find(rawVkCode) != g_hotkeyMainKeys.end()) ||
                          (g_hotkeyMainKeys.find(vkCode) != g_hotkeyMainKeys.end());
    }

    if (!isHotkeyMainKey) {
        // This key is not a hotkey main key, but it might invalidate pending trigger-on-release hotkeys
        if (isKeyDown) {
            std::lock_guard<std::mutex> lock(g_triggerOnReleaseMutex);
            // Any key press (that's not a hotkey) invalidates ALL pending trigger-on-release hotkeys
            for (const auto& pendingHotkeyId : g_triggerOnReleasePending) { g_triggerOnReleaseInvalidated.insert(pendingHotkeyId); }
        }
        // Do not return "consumed" here.
        return { false, 0 };
    }

    // Use config snapshot for thread-safe hotkey iteration
    auto cfgSnap = GetConfigSnapshot();
    if (!cfgSnap) { return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) }; }
    const Config& cfg = *cfgSnap;

    DWORD rebindTargetVk = 0;
    if (cfg.keyRebinds.enabled) {
        for (const auto& rebind : cfg.keyRebinds.rebinds) {
            if (rebind.enabled && rebind.fromKey != 0 && rebind.toKey != 0 &&
                (vkCode == rebind.fromKey || rawVkCode == rebind.fromKey)) {
                rebindTargetVk = (rebind.useCustomOutput && rebind.customOutputVK != 0)
                    ? rebind.customOutputVK : rebind.toKey;
                break;
            }
        }
    }

    bool s_enableHotkeyDebug = cfg.debug.showHotkeyDebug;

    if (s_enableHotkeyDebug) {
        Log("[Hotkey] Key/button pressed: " + std::to_string(vkCode) + " (raw=" + std::to_string(rawVkCode) + ") in mode: " +
            currentModeId);
    }
    if (s_enableHotkeyDebug) {
        Log("[Hotkey] Current game state: " + gameState);
        Log("[Hotkey] Evaluating " + std::to_string(cfg.hotkeys.size()) + " configured hotkeys");
    }

    for (size_t hotkeyIdx = 0; hotkeyIdx < cfg.hotkeys.size(); ++hotkeyIdx) {
        const auto& hotkey = cfg.hotkeys[hotkeyIdx];
        if (s_enableHotkeyDebug) {
            Log("[Hotkey] Checking: " + GetKeyComboString(hotkey.keys) + " (main: " + hotkey.mainMode + ", sec: " + hotkey.secondaryMode +
                ")");
        }

        bool conditionsMet = hotkey.conditions.gameState.empty() ||
                             std::find(hotkey.conditions.gameState.begin(), hotkey.conditions.gameState.end(), gameState) !=
                                 hotkey.conditions.gameState.end();

        std::string currentSecMode;
        bool wouldExitToFullscreen = false;
        if (hotkey.allowExitToFullscreenRegardlessOfGameState) {
            currentSecMode = GetHotkeySecondaryMode(hotkeyIdx);
            wouldExitToFullscreen = !currentSecMode.empty() && EqualsIgnoreCase(currentModeId, currentSecMode);
        }

        if (!conditionsMet) {
            if (!(hotkey.allowExitToFullscreenRegardlessOfGameState && wouldExitToFullscreen)) {
                if (s_enableHotkeyDebug) { Log("[Hotkey] SKIP: Game state conditions not met"); }
                continue;
            }
            if (s_enableHotkeyDebug) {
                Log("[Hotkey] BYPASS: Allowing exit to Fullscreen even though game state conditions are not met");
            }
        }

        for (const auto& alt : hotkey.altSecondaryModes) {
            bool matched = CheckHotkeyMatch(alt.keys, vkCode, hotkey.conditions.exclusions, hotkey.triggerOnRelease);
            bool matchedViaRebind = !matched && rebindTargetVk && CheckHotkeyMatch(alt.keys, rebindTargetVk, hotkey.conditions.exclusions, hotkey.triggerOnRelease);
            if (matched || matchedViaRebind) {
                // When matched via rebind target, always block original key from game
                bool blockKey = hotkey.blockKeyFromGame || matchedViaRebind;
                std::string hotkeyId = GetKeyComboString(alt.keys);

                // Handle trigger-on-release invalidation tracking
                if (hotkey.triggerOnRelease) {
                    if (isKeyDown) {
                        std::lock_guard<std::mutex> lock(g_triggerOnReleaseMutex);
                        for (const auto& pendingHotkeyId : g_triggerOnReleasePending) {
                            if (pendingHotkeyId != hotkeyId) { g_triggerOnReleaseInvalidated.insert(pendingHotkeyId); }
                        }
                        g_triggerOnReleasePending.insert(hotkeyId);
                        if (s_enableHotkeyDebug) { Log("[Hotkey] Alt trigger-on-release hotkey pressed, added to pending: " + hotkeyId); }
                        if (blockKey) return { true, 0 };
                        return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) };
                    } else {
                        // Key released - check if invalidated
                        bool wasInvalidated = false;
                        {
                            std::lock_guard<std::mutex> lock(g_triggerOnReleaseMutex);
                            wasInvalidated = g_triggerOnReleaseInvalidated.count(hotkeyId) > 0;
                            g_triggerOnReleasePending.erase(hotkeyId);
                            g_triggerOnReleaseInvalidated.erase(hotkeyId);
                        }

                        if (wasInvalidated) {
                            if (s_enableHotkeyDebug) { Log("[Hotkey] Alt trigger-on-release hotkey invalidated: " + hotkeyId); }
                            if (blockKey) return { true, 0 };
                            return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) };
                        }
                    }
                }

                // Check if this hotkey should trigger based on triggerOnRelease setting
                // When triggerOnRelease is true, only fire on key UP; when false (default), only fire on key DOWN
                if (hotkey.triggerOnRelease != isKeyDown) {
                    auto now = std::chrono::steady_clock::now();
                    bool debounced = false;
                    {
                        std::lock_guard<std::mutex> tsLock(g_hotkeyTimestampsMutex);
                        auto it = g_hotkeyTimestamps.find(hotkeyId);
                        if (it != g_hotkeyTimestamps.end() &&
                            std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second).count() < hotkey.debounce) {
                            debounced = true;
                        } else {
                            g_hotkeyTimestamps[hotkeyId] = now;
                        }
                    }
                    if (debounced) {
                        if (s_enableHotkeyDebug) { Log("[Hotkey] Alt hotkey matched but debounced: " + hotkeyId); }
                        if (blockKey) return { true, 0 };
                        return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) };
                    }

                    std::string currentSecMode = GetHotkeySecondaryMode(hotkeyIdx);
                    std::string newSecMode = (currentSecMode == alt.mode) ? hotkey.secondaryMode : alt.mode;
                    SetHotkeySecondaryMode(hotkeyIdx, newSecMode);

                    if (s_enableHotkeyDebug) { Log("[Hotkey] ✓✓✓ ALT HOTKEY TRIGGERED: " + hotkeyId + " -> " + newSecMode); }

                    if (!newSecMode.empty()) { SwitchToMode(newSecMode, "alt hotkey"); }
                }
                if (blockKey) return { true, 0 };
                return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) };
            }
        }

        {
            bool matched = CheckHotkeyMatch(hotkey.keys, vkCode, hotkey.conditions.exclusions, hotkey.triggerOnRelease);
            bool matchedViaRebind = !matched && rebindTargetVk && CheckHotkeyMatch(hotkey.keys, rebindTargetVk, hotkey.conditions.exclusions, hotkey.triggerOnRelease);
            if (matched || matchedViaRebind) {
                // When matched via rebind target, always block original key from game
                bool blockKey = hotkey.blockKeyFromGame || matchedViaRebind;
                std::string hotkeyId = GetKeyComboString(hotkey.keys);

                // Handle trigger-on-release invalidation tracking
                if (hotkey.triggerOnRelease) {
                    if (isKeyDown) {
                        std::lock_guard<std::mutex> lock(g_triggerOnReleaseMutex);
                        // Invalidate all other pending trigger-on-release hotkeys
                        for (const auto& pendingHotkeyId : g_triggerOnReleasePending) {
                            if (pendingHotkeyId != hotkeyId) { g_triggerOnReleaseInvalidated.insert(pendingHotkeyId); }
                        }
                        g_triggerOnReleasePending.insert(hotkeyId);
                        if (s_enableHotkeyDebug) { Log("[Hotkey] Trigger-on-release hotkey pressed, added to pending: " + hotkeyId); }
                        if (blockKey) return { true, 0 };
                        return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) };
                    } else {
                        // Key released - check if invalidated
                        bool wasInvalidated = false;
                        {
                            std::lock_guard<std::mutex> lock(g_triggerOnReleaseMutex);
                            wasInvalidated = g_triggerOnReleaseInvalidated.count(hotkeyId) > 0;
                            g_triggerOnReleasePending.erase(hotkeyId);
                            g_triggerOnReleaseInvalidated.erase(hotkeyId);
                        }

                        if (wasInvalidated) {
                            if (s_enableHotkeyDebug) {
                                Log("[Hotkey] Trigger-on-release hotkey invalidated (another key was pressed): " + hotkeyId);
                            }
                            if (blockKey) return { true, 0 };
                            return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) };
                        }
                    }
                }

                // Check if this hotkey should trigger based on triggerOnRelease setting
                // When triggerOnRelease is true, only fire on key UP; when false (default), only fire on key DOWN
                if (hotkey.triggerOnRelease != isKeyDown) {
                    auto now = std::chrono::steady_clock::now();
                    bool debounced = false;
                    {
                        std::lock_guard<std::mutex> tsLock(g_hotkeyTimestampsMutex);
                        auto it = g_hotkeyTimestamps.find(hotkeyId);
                        if (it != g_hotkeyTimestamps.end() &&
                            std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second).count() < hotkey.debounce) {
                            debounced = true;
                        } else {
                            g_hotkeyTimestamps[hotkeyId] = now;
                        }
                    }
                    if (debounced) {
                        if (s_enableHotkeyDebug) { Log("[Hotkey] Main hotkey matched but debounced: " + hotkeyId); }
                        if (blockKey) return { true, 0 };
                        return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) };
                    }

                    // Lock-free read of current mode ID from double-buffer
                    std::string current = g_modeIdBuffers[g_currentModeIdIndex.load(std::memory_order_acquire)];
                    std::string targetMode;

                    if (currentSecMode.empty()) {
                        currentSecMode = GetHotkeySecondaryMode(hotkeyIdx);
                    }

                    if (EqualsIgnoreCase(current, currentSecMode)) {
                        targetMode = cfg.defaultMode;
                    } else {
                        targetMode = currentSecMode;
                    }

                    if (s_enableHotkeyDebug) {
                        Log("[Hotkey] ✓✓✓ MAIN HOTKEY TRIGGERED: " + hotkeyId + " (current: " + current + " -> target: " + targetMode + ")");
                    }

                    if (!targetMode.empty()) { SwitchToMode(targetMode, "main hotkey"); }
                }
                if (blockKey) return { true, 0 };
                return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) };
            }
        }
    }

    for (size_t sensIdx = 0; sensIdx < cfg.sensitivityHotkeys.size(); ++sensIdx) {
        const auto& sensHotkey = cfg.sensitivityHotkeys[sensIdx];
        if (s_enableHotkeyDebug) {
            Log("[Hotkey] Checking sensitivity hotkey: " + GetKeyComboString(sensHotkey.keys) +
                " -> sens=" + std::to_string(sensHotkey.sensitivity));
        }

        bool conditionsMet = sensHotkey.conditions.gameState.empty() ||
                             std::find(sensHotkey.conditions.gameState.begin(), sensHotkey.conditions.gameState.end(), gameState) !=
                                 sensHotkey.conditions.gameState.end();
        if (!conditionsMet) {
            if (s_enableHotkeyDebug) { Log("[Hotkey] SKIP sensitivity: Game state conditions not met"); }
            continue;
        }

        // Sensitivity hotkeys only trigger on key down (no triggerOnRelease support)
        if (!isKeyDown) { continue; }

        {
            bool matched = CheckHotkeyMatch(sensHotkey.keys, vkCode, sensHotkey.conditions.exclusions, false);
            bool matchedViaRebind = !matched && rebindTargetVk && CheckHotkeyMatch(sensHotkey.keys, rebindTargetVk, sensHotkey.conditions.exclusions, false);
            if (matched || matchedViaRebind) {
                // Sensitivity hotkeys have no blockKeyFromGame setting; only block when matched via rebind
                bool blockKey = matchedViaRebind;
                std::string hotkeyId = "sens_" + GetKeyComboString(sensHotkey.keys);

                auto now = std::chrono::steady_clock::now();
                bool debounced = false;
                {
                    std::lock_guard<std::mutex> tsLock(g_hotkeyTimestampsMutex);
                    auto it = g_hotkeyTimestamps.find(hotkeyId);
                    if (it != g_hotkeyTimestamps.end() &&
                        std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second).count() < sensHotkey.debounce) {
                        debounced = true;
                    } else {
                        g_hotkeyTimestamps[hotkeyId] = now;
                    }
                }
                if (debounced) {
                    if (s_enableHotkeyDebug) { Log("[Hotkey] Sensitivity hotkey matched but debounced: " + hotkeyId); }
                    if (blockKey) return { true, 0 };
                    return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) };
                }

                if (sensHotkey.toggle) {
                    extern TempSensitivityOverride g_tempSensitivityOverride;
                    extern std::mutex g_tempSensitivityMutex;
                    std::lock_guard<std::mutex> lock(g_tempSensitivityMutex);

                    if (g_tempSensitivityOverride.active && g_tempSensitivityOverride.activeSensHotkeyIndex == static_cast<int>(sensIdx)) {
                        g_tempSensitivityOverride.active = false;
                        g_tempSensitivityOverride.sensitivityX = 1.0f;
                        g_tempSensitivityOverride.sensitivityY = 1.0f;
                        g_tempSensitivityOverride.activeSensHotkeyIndex = -1;

                        if (s_enableHotkeyDebug) { Log("[Hotkey] ✓✓✓ SENSITIVITY HOTKEY TOGGLED OFF: " + hotkeyId); }

                        if (blockKey) return { true, 0 };
                        return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) };
                    }

                    g_tempSensitivityOverride.active = true;
                    if (sensHotkey.separateXY) {
                        g_tempSensitivityOverride.sensitivityX = sensHotkey.sensitivityX;
                        g_tempSensitivityOverride.sensitivityY = sensHotkey.sensitivityY;
                    } else {
                        g_tempSensitivityOverride.sensitivityX = sensHotkey.sensitivity;
                        g_tempSensitivityOverride.sensitivityY = sensHotkey.sensitivity;
                    }
                    g_tempSensitivityOverride.activeSensHotkeyIndex = static_cast<int>(sensIdx);

                    if (s_enableHotkeyDebug) {
                        Log("[Hotkey] ✓✓✓ SENSITIVITY HOTKEY TOGGLED ON: " + hotkeyId + " -> sens=" + std::to_string(sensHotkey.sensitivity));
                    }
                } else {
                    {
                        extern TempSensitivityOverride g_tempSensitivityOverride;
                        extern std::mutex g_tempSensitivityMutex;
                        std::lock_guard<std::mutex> lock(g_tempSensitivityMutex);
                        g_tempSensitivityOverride.active = true;
                        if (sensHotkey.separateXY) {
                            g_tempSensitivityOverride.sensitivityX = sensHotkey.sensitivityX;
                            g_tempSensitivityOverride.sensitivityY = sensHotkey.sensitivityY;
                        } else {
                            g_tempSensitivityOverride.sensitivityX = sensHotkey.sensitivity;
                            g_tempSensitivityOverride.sensitivityY = sensHotkey.sensitivity;
                        }
                        g_tempSensitivityOverride.activeSensHotkeyIndex = -1;
                    }

                    if (s_enableHotkeyDebug) {
                        Log("[Hotkey] ✓✓✓ SENSITIVITY HOTKEY TRIGGERED: " + hotkeyId + " -> sens=" + std::to_string(sensHotkey.sensitivity));
                    }
                }

                if (blockKey) return { true, 0 };
                return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) };
            }
        }
    }

    return { false, 0 };
}

InputHandlerResult HandleMouseCoordinateTranslationPhase(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM& lParam) {
    // Only translate messages whose lParam is already in CLIENT coordinates.
    // Wheel messages use SCREEN coordinates and must not be transformed here.
    switch (uMsg) {
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_XBUTTONDBLCLK:
        break;
    default:
        return { false, 0 };
    }

    PROFILE_SCOPE("HandleMouseCoordinateTranslation");

    ModeViewportInfo geo{};
    const CachedModeViewport& cachedMode = g_viewportModeCache[g_viewportModeCacheIndex.load(std::memory_order_acquire)];
    if (cachedMode.valid) {
        geo.valid = true;
        geo.width = cachedMode.width;
        geo.height = cachedMode.height;
        geo.stretchEnabled = cachedMode.stretchEnabled;
        geo.stretchX = cachedMode.stretchX;
        geo.stretchY = cachedMode.stretchY;
        geo.stretchWidth = cachedMode.stretchWidth;
        geo.stretchHeight = cachedMode.stretchHeight;
    } else {
        geo = GetCurrentModeViewport();
    }
    if (!geo.valid || geo.width <= 0 || geo.height <= 0 || geo.stretchWidth <= 0 || geo.stretchHeight <= 0) { return { false, 0 }; }

    RECT clientRect{};
    if (!GetClientRect(hWnd, &clientRect)) { return { false, 0 }; }
    const int clientW = clientRect.right - clientRect.left;
    const int clientH = clientRect.bottom - clientRect.top;
    if (clientW <= 0 || clientH <= 0) { return { false, 0 }; }

    const int viewportLeft = geo.stretchX;
    const int viewportTop = geo.stretchY;
    const int viewportRight = geo.stretchX + geo.stretchWidth;
    const int viewportBottom = geo.stretchY + geo.stretchHeight;

    const int visibleLeft = (std::max)(0, viewportLeft);
    const int visibleTop = (std::max)(0, viewportTop);
    const int visibleRight = (std::min)(clientW, viewportRight);
    const int visibleBottom = (std::min)(clientH, viewportBottom);

    const int visibleW = visibleRight - visibleLeft;
    const int visibleH = visibleBottom - visibleTop;
    if (visibleW <= 0 || visibleH <= 0) { return { false, 0 }; }

    int mouseX = GET_X_LPARAM(lParam);
    int mouseY = GET_Y_LPARAM(lParam);

    if (mouseX < visibleLeft) mouseX = visibleLeft;
    if (mouseX >= visibleRight) mouseX = visibleRight - 1;
    if (mouseY < visibleTop) mouseY = visibleTop;
    if (mouseY >= visibleBottom) mouseY = visibleBottom - 1;

    const float scaleX = static_cast<float>(geo.width) / static_cast<float>(geo.stretchWidth);
    const float scaleY = static_cast<float>(geo.height) / static_cast<float>(geo.stretchHeight);

    const float srcOffsetX = static_cast<float>(visibleLeft - viewportLeft) * scaleX;
    const float srcOffsetY = static_cast<float>(visibleTop - viewportTop) * scaleY;

    const float localVisibleX = static_cast<float>(mouseX - visibleLeft);
    const float localVisibleY = static_cast<float>(mouseY - visibleTop);

    int newX = static_cast<int>(srcOffsetX + localVisibleX * scaleX);
    int newY = static_cast<int>(srcOffsetY + localVisibleY * scaleY);

    if (newX < 0) newX = 0;
    if (newY < 0) newY = 0;
    if (newX >= geo.width) newX = geo.width - 1;
    if (newY >= geo.height) newY = geo.height - 1;

    lParam = MAKELPARAM(newX, newY);
    return { false, 0 };
}

static UINT GetScanCodeWithExtendedFlag(DWORD vkCode) {
    auto isExtendedVk = [](DWORD vk) {
        switch (vk) {
        case VK_LEFT:
        case VK_RIGHT:
        case VK_UP:
        case VK_DOWN:
        case VK_INSERT:
        case VK_DELETE:
        case VK_HOME:
        case VK_END:
        case VK_PRIOR:
        case VK_NEXT:
        case VK_RCONTROL:
        case VK_RMENU:
        case VK_DIVIDE:
        case VK_NUMLOCK:
        case VK_SNAPSHOT:
            return true;
        default:
            return false;
        }
    };

    UINT scanCodeWithFlags = MapVirtualKey(static_cast<UINT>(vkCode), MAPVK_VK_TO_VSC_EX);
    if (scanCodeWithFlags == 0) {
        scanCodeWithFlags = MapVirtualKey(static_cast<UINT>(vkCode), MAPVK_VK_TO_VSC);
    }

    if ((scanCodeWithFlags & 0xFF00) == 0 && isExtendedVk(vkCode) && (scanCodeWithFlags & 0xFF) != 0) { scanCodeWithFlags |= 0xE000; }

    return scanCodeWithFlags;
}

static LPARAM BuildKeyboardMessageLParam(UINT scanCodeWithFlags, bool isKeyDown, bool isSystemKey, UINT repeatCount, bool previousKeyState,
                                         bool transitionState) {
    const UINT scanLow = scanCodeWithFlags & 0xFF;
    const bool isExtended = (scanCodeWithFlags & 0xFF00) != 0;

    LPARAM out = static_cast<LPARAM>(repeatCount == 0 ? 1 : repeatCount);
    out |= (static_cast<LPARAM>(scanLow) << 16);
    if (isExtended) out |= (1LL << 24);
    if (isSystemKey) out |= (1LL << 29);
    if (previousKeyState) out |= (1LL << 30);
    if (transitionState) out |= (1LL << 31);

    if (!isKeyDown) out |= (1LL << 30) | (1LL << 31);

    return out;
}

static UINT ResolveOutputScanCode(DWORD outputVk, UINT configuredScanCodeWithFlags) {
    if (configuredScanCodeWithFlags == 0) { return GetScanCodeWithExtendedFlag(outputVk); }

    if ((configuredScanCodeWithFlags & 0xFF00) == 0) {
        UINT vkScan = GetScanCodeWithExtendedFlag(outputVk);
        if ((vkScan & 0xFF00) != 0 && ((vkScan & 0xFF) == (configuredScanCodeWithFlags & 0xFF))) { return vkScan; }
    }

    return configuredScanCodeWithFlags;
}

static bool TryTranslateVkToChar(DWORD vkCode, bool shiftDown, WCHAR& outChar) {
    BYTE keyboardState[256] = {};
    if (shiftDown) keyboardState[VK_SHIFT] = 0x80;

    HKL keyboardLayout = GetKeyboardLayout(0);
    UINT scanCode = GetScanCodeWithExtendedFlag(vkCode) & 0xFF;
    WCHAR utf16Buffer[8] = {};

    int translated = ToUnicodeEx(static_cast<UINT>(vkCode), scanCode, keyboardState, utf16Buffer, 8, 0, keyboardLayout);
    if (translated == 1) {
        outChar = utf16Buffer[0];
        return outChar != 0;
    }

    if (translated < 0) {
        BYTE emptyState[256] = {};
        WCHAR clearBuffer[8] = {};
        ToUnicodeEx(static_cast<UINT>(vkCode), scanCode, emptyState, clearBuffer, 8, 0, keyboardLayout);
    }

    return false;
}

static bool TryTranslateVkToCharWithKeyboardState(DWORD vkCode, const BYTE keyboardState[256], WCHAR& outChar) {
    HKL keyboardLayout = GetKeyboardLayout(0);
    UINT scanCode = GetScanCodeWithExtendedFlag(vkCode) & 0xFF;

    WCHAR utf16Buffer[8] = {};
    BYTE ksCopy[256] = {};
    memcpy(ksCopy, keyboardState, 256);

    int translated = ToUnicodeEx(static_cast<UINT>(vkCode), scanCode, ksCopy, utf16Buffer, 8, 0, keyboardLayout);
    if (translated == 1) {
        outChar = utf16Buffer[0];
        return outChar != 0;
    }

    if (translated < 0) {
        BYTE emptyState[256] = {};
        WCHAR clearBuffer[8] = {};
        ToUnicodeEx(static_cast<UINT>(vkCode), scanCode, emptyState, clearBuffer, 8, 0, keyboardLayout);
    }

    return false;
}

static bool IsValidUnicodeScalar(uint32_t cp) {
    if (cp == 0) return false;
    if (cp > 0x10FFFFu) return false;
    if (cp >= 0xD800u && cp <= 0xDFFFu) return false;
    return true;
}

static LRESULT SendUnicodeScalarAsCharMessage(HWND hWnd, UINT charMsg, uint32_t cp, LPARAM lParam) {
    if (!IsValidUnicodeScalar(cp)) return 0;

    if (cp <= 0xFFFFu) {
        return CallWindowProc(g_originalWndProc, hWnd, charMsg, (WPARAM)(WCHAR)cp, lParam);
    }

    uint32_t v = cp - 0x10000u;
    WCHAR high = (WCHAR)(0xD800u + (v >> 10));
    WCHAR low = (WCHAR)(0xDC00u + (v & 0x3FFu));
    (void)CallWindowProc(g_originalWndProc, hWnd, charMsg, (WPARAM)high, lParam);
    return CallWindowProc(g_originalWndProc, hWnd, charMsg, (WPARAM)low, lParam);
}

static constexpr ULONG_PTR kToolscreenInjectedExtraInfo = (ULONG_PTR)0x5453434E;

static bool SendSynthKeyByScanCode(UINT scanCodeWithFlags, bool keyDown) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = 0;
    in.ki.wScan = (WORD)(scanCodeWithFlags & 0xFF);
    DWORD flags = KEYEVENTF_SCANCODE;
    if ((scanCodeWithFlags & 0xFF00) != 0) flags |= KEYEVENTF_EXTENDEDKEY;
    if (!keyDown) flags |= KEYEVENTF_KEYUP;
    in.ki.dwFlags = flags;
    in.ki.time = 0;
    in.ki.dwExtraInfo = kToolscreenInjectedExtraInfo;
    return ::SendInput(1, &in, sizeof(INPUT)) == 1;
}

InputHandlerResult HandleKeyRebinding(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
        break;
    default:
        return { false, 0 };
    }
    PROFILE_SCOPE("HandleKeyRebinding");

    if (uMsg == WM_KEYDOWN || uMsg == WM_SYSKEYDOWN || uMsg == WM_KEYUP || uMsg == WM_SYSKEYUP) {
        if (GetMessageExtraInfo() == kToolscreenInjectedExtraInfo) {
            return { false, 0 };
        }
    }

    DWORD rawVkCode = 0;
    DWORD vkCode = 0;
    bool isMouseButton = false;
    bool isKeyDown = false;

    if (uMsg == WM_KEYDOWN || uMsg == WM_SYSKEYDOWN) {
        rawVkCode = static_cast<DWORD>(wParam);
        isKeyDown = true;
    } else if (uMsg == WM_KEYUP || uMsg == WM_SYSKEYUP) {
        rawVkCode = static_cast<DWORD>(wParam);
        isKeyDown = false;
    } else if (uMsg == WM_XBUTTONDOWN) {
        WORD xButton = GET_XBUTTON_WPARAM(wParam);
        rawVkCode = (xButton == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;
        isMouseButton = true;
        isKeyDown = true;
    } else if (uMsg == WM_XBUTTONUP) {
        WORD xButton = GET_XBUTTON_WPARAM(wParam);
        rawVkCode = (xButton == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;
        isMouseButton = true;
        isKeyDown = false;
    } else if (uMsg == WM_LBUTTONDOWN) {
        rawVkCode = VK_LBUTTON;
        isMouseButton = true;
        isKeyDown = true;
    } else if (uMsg == WM_LBUTTONUP) {
        rawVkCode = VK_LBUTTON;
        isMouseButton = true;
        isKeyDown = false;
    } else if (uMsg == WM_RBUTTONDOWN) {
        rawVkCode = VK_RBUTTON;
        isMouseButton = true;
        isKeyDown = true;
    } else if (uMsg == WM_RBUTTONUP) {
        rawVkCode = VK_RBUTTON;
        isMouseButton = true;
        isKeyDown = false;
    } else if (uMsg == WM_MBUTTONDOWN) {
        rawVkCode = VK_MBUTTON;
        isMouseButton = true;
        isKeyDown = true;
    } else if (uMsg == WM_MBUTTONUP) {
        rawVkCode = VK_MBUTTON;
        isMouseButton = true;
        isKeyDown = false;
    } else {
        return { false, 0 };
    }

    if (isMouseButton && g_showGui.load(std::memory_order_acquire)) { return { false, 0 }; }

    vkCode = rawVkCode;
    if (!isMouseButton && (uMsg == WM_KEYDOWN || uMsg == WM_SYSKEYDOWN || uMsg == WM_KEYUP || uMsg == WM_SYSKEYUP)) {
        vkCode = NormalizeModifierVkFromKeyMessage(rawVkCode, lParam);
        if (vkCode == 0) vkCode = rawVkCode;
    }

    const bool isAutoRepeatKeyDown = (!isMouseButton && isKeyDown && (uMsg == WM_KEYDOWN || uMsg == WM_SYSKEYDOWN) && ((lParam & (1LL << 30)) != 0));

    // Use config snapshot for thread-safe access to key rebinds
    auto rebindCfg = GetConfigSnapshot();
    if (!rebindCfg || !rebindCfg->keyRebinds.enabled) { return { false, 0 }; }

    auto matchesFromKey = [&](DWORD incomingVk, DWORD incomingRawVk, DWORD fromKey) -> bool {
        if (fromKey == 0) return false;
        if (incomingVk == fromKey) return true;

        if (fromKey == VK_CONTROL) {
            return incomingVk == VK_LCONTROL || incomingVk == VK_RCONTROL || incomingRawVk == VK_CONTROL;
        }
        if (fromKey == VK_SHIFT) {
            return incomingVk == VK_LSHIFT || incomingVk == VK_RSHIFT || incomingRawVk == VK_SHIFT;
        }
        if (fromKey == VK_MENU) {
            return incomingVk == VK_LMENU || incomingVk == VK_RMENU || incomingRawVk == VK_MENU;
        }

        if (incomingRawVk == VK_CONTROL && incomingVk == VK_CONTROL && (fromKey == VK_LCONTROL || fromKey == VK_RCONTROL)) return true;
        if (incomingRawVk == VK_SHIFT && incomingVk == VK_SHIFT && (fromKey == VK_LSHIFT || fromKey == VK_RSHIFT)) return true;
        if (incomingRawVk == VK_MENU && incomingVk == VK_MENU && (fromKey == VK_LMENU || fromKey == VK_RMENU)) return true;

        return false;
    };

    for (size_t i = 0; i < rebindCfg->keyRebinds.rebinds.size(); ++i) {
        const auto& rebind = rebindCfg->keyRebinds.rebinds[i];

        if (rebind.enabled && rebind.fromKey != 0 && rebind.toKey != 0 && matchesFromKey(vkCode, rawVkCode, rebind.fromKey)) {
            auto isModifierVk = [](DWORD vk) {
                return vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT ||
                       vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU || vk == VK_LWIN || vk == VK_RWIN;
            };

            const DWORD triggerVK =
                NormalizeModifierVkFromConfig(rebind.toKey, (rebind.useCustomOutput ? rebind.customOutputScanCode : 0));

            const DWORD textVK = NormalizeModifierVkFromConfig(
                (rebind.useCustomOutput && rebind.customOutputVK != 0) ? rebind.customOutputVK : triggerVK);

            UINT outputScanCode = GetScanCodeWithExtendedFlag(triggerVK);
            if (rebind.useCustomOutput && rebind.customOutputScanCode != 0) {
                outputScanCode = ResolveOutputScanCode(triggerVK, rebind.customOutputScanCode);
            }

            if (triggerVK == VK_LBUTTON || triggerVK == VK_RBUTTON || triggerVK == VK_MBUTTON || triggerVK == VK_XBUTTON1 ||
                triggerVK == VK_XBUTTON2) {
                UINT newMsg = 0;
                auto buildMouseKeyState = [&](DWORD buttonVk, bool buttonDown) -> WORD {
                    WORD mk = 0;
                    if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) mk |= MK_CONTROL;
                    if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) mk |= MK_SHIFT;

                    auto setBtn = [&](int vk, WORD mask, bool isThisButton) {
                        bool down = (GetKeyState(vk) & 0x8000) != 0;
                        if (isThisButton) down = buttonDown;
                        if (down) mk |= mask;
                    };

                    setBtn(VK_LBUTTON, MK_LBUTTON, buttonVk == VK_LBUTTON);
                    setBtn(VK_RBUTTON, MK_RBUTTON, buttonVk == VK_RBUTTON);
                    setBtn(VK_MBUTTON, MK_MBUTTON, buttonVk == VK_MBUTTON);
                    setBtn(VK_XBUTTON1, MK_XBUTTON1, buttonVk == VK_XBUTTON1);
                    setBtn(VK_XBUTTON2, MK_XBUTTON2, buttonVk == VK_XBUTTON2);
                    return mk;
                };

                LPARAM mouseLParam = lParam;
                if (!isMouseButton) {
                    POINT pt{};
                    if (GetCursorPos(&pt) && ScreenToClient(hWnd, &pt)) {
                        mouseLParam = MAKELPARAM(pt.x, pt.y);
                    } else {
                        RECT clientRect{};
                        if (GetClientRect(hWnd, &clientRect)) {
                            mouseLParam = MAKELPARAM((clientRect.right - clientRect.left) / 2, (clientRect.bottom - clientRect.top) / 2);
                        }
                    }
                }

                WORD mkState = buildMouseKeyState(triggerVK, isKeyDown);
                WPARAM newWParam = mkState;

                if (triggerVK == VK_LBUTTON) {
                    newMsg = isKeyDown ? WM_LBUTTONDOWN : WM_LBUTTONUP;
                } else if (triggerVK == VK_RBUTTON) {
                    newMsg = isKeyDown ? WM_RBUTTONDOWN : WM_RBUTTONUP;
                } else if (triggerVK == VK_MBUTTON) {
                    newMsg = isKeyDown ? WM_MBUTTONDOWN : WM_MBUTTONUP;
                } else if (triggerVK == VK_XBUTTON1) {
                    newMsg = isKeyDown ? WM_XBUTTONDOWN : WM_XBUTTONUP;
                    newWParam = MAKEWPARAM(mkState, XBUTTON1);
                } else if (triggerVK == VK_XBUTTON2) {
                    newMsg = isKeyDown ? WM_XBUTTONDOWN : WM_XBUTTONUP;
                    newWParam = MAKEWPARAM(mkState, XBUTTON2);
                }

                LRESULT mouseResult = CallWindowProc(g_originalWndProc, hWnd, newMsg, newWParam, mouseLParam);

                const bool fromKeyIsNonCharMouse =
                    isMouseButton ||
                    isModifierVk(rebind.fromKey) || rebind.fromKey == VK_LWIN || rebind.fromKey == VK_RWIN ||
                    (rebind.fromKey >= VK_F1 && rebind.fromKey <= VK_F24);

                if (isKeyDown && fromKeyIsNonCharMouse) {
                    const uint32_t configuredUnicodeText =
                        (rebind.useCustomOutput && rebind.customOutputUnicode != 0) ? (uint32_t)rebind.customOutputUnicode : 0u;

                    const UINT textScanCode = GetScanCodeWithExtendedFlag(textVK);
                    LPARAM charLParam = BuildKeyboardMessageLParam(textScanCode, true, false, 1, false, false);

                    if (configuredUnicodeText != 0) {
                        SendMessage(hWnd, WM_TOOLSCREEN_CHAR_NO_REBIND, (WPARAM)(WCHAR)configuredUnicodeText, charLParam);
                    } else {
                        WCHAR outChar = 0;
                        if (textVK == VK_RETURN) {
                            outChar = L'\r';
                        } else if (textVK == VK_TAB) {
                            outChar = L'\t';
                        } else if (textVK == VK_BACK) {
                            outChar = L'\b';
                        } else {
                            BYTE ks[256] = {};
                            if (GetKeyboardState(ks)) {
                                if (rebind.fromKey == VK_SHIFT || rebind.fromKey == VK_LSHIFT || rebind.fromKey == VK_RSHIFT) {
                                    ks[VK_SHIFT] = 0;
                                    ks[VK_LSHIFT] = 0;
                                    ks[VK_RSHIFT] = 0;
                                } else if (rebind.fromKey == VK_CONTROL || rebind.fromKey == VK_LCONTROL || rebind.fromKey == VK_RCONTROL) {
                                    ks[VK_CONTROL] = 0;
                                    ks[VK_LCONTROL] = 0;
                                    ks[VK_RCONTROL] = 0;
                                } else if (rebind.fromKey == VK_MENU || rebind.fromKey == VK_LMENU || rebind.fromKey == VK_RMENU) {
                                    ks[VK_MENU] = 0;
                                    ks[VK_LMENU] = 0;
                                    ks[VK_RMENU] = 0;
                                }

                                (void)TryTranslateVkToCharWithKeyboardState(textVK, ks, outChar);
                            }

                            if (outChar == 0) {
                                if (!TryTranslateVkToChar(textVK, false, outChar) || outChar == 0) {
                                    (void)TryTranslateVkToChar(textVK, true, outChar);
                                }
                            }
                        }

                        if (outChar != 0) {
                            SendMessage(hWnd, WM_TOOLSCREEN_CHAR_NO_REBIND, static_cast<WPARAM>(outChar), charLParam);
                        }
                    }
                }

                return { true, mouseResult };
            }

            const bool isSystemKeyMsg = (uMsg == WM_SYSKEYDOWN || uMsg == WM_SYSKEYUP);
            const bool outputIsAlt = (triggerVK == VK_MENU || triggerVK == VK_LMENU || triggerVK == VK_RMENU);
            const bool useSysKey = isSystemKeyMsg || outputIsAlt;
            UINT outputMsg = isKeyDown ? (useSysKey ? WM_SYSKEYDOWN : WM_KEYDOWN) : (useSysKey ? WM_SYSKEYUP : WM_KEYUP);

            const bool fromKeyIsNonChar =
                isMouseButton ||
                isModifierVk(rebind.fromKey) || rebind.fromKey == VK_LWIN || rebind.fromKey == VK_RWIN ||
                (rebind.fromKey >= VK_F1 && rebind.fromKey <= VK_F24);

            UINT repeatCount = 1;
            bool previousState = !isKeyDown;
            bool transitionState = !isKeyDown;
            if (!isMouseButton) {
                repeatCount = static_cast<UINT>(lParam & 0xFFFF);
                if (repeatCount == 0) repeatCount = 1;

                previousState = ((lParam & (1LL << 30)) != 0);
                transitionState = ((lParam & (1LL << 31)) != 0);
            }

            auto emitTypedChar = [&](LPARAM charLParam) {
                const uint32_t configuredUnicodeText =
                    (rebind.useCustomOutput && rebind.customOutputUnicode != 0) ? (uint32_t)rebind.customOutputUnicode : 0u;

                if (configuredUnicodeText != 0) {
                    const UINT charMsg = isSystemKeyMsg ? WM_SYSCHAR : WM_CHAR;
                    if (charMsg == WM_CHAR && configuredUnicodeText <= 0xFFFFu) {
                        SendMessage(hWnd, WM_TOOLSCREEN_CHAR_NO_REBIND, (WPARAM)(WCHAR)configuredUnicodeText, charLParam);
                    } else {
                        SendUnicodeScalarAsCharMessage(hWnd, charMsg, configuredUnicodeText, charLParam);
                    }
                    return;
                }

                WCHAR outChar = 0;

                if (textVK == VK_RETURN) {
                    outChar = L'\r';
                } else if (textVK == VK_TAB) {
                    outChar = L'\t';
                } else if (textVK == VK_BACK) {
                    outChar = L'\b';
                } else {
                    BYTE ks[256] = {};
                    if (GetKeyboardState(ks)) {
                        if (rebind.fromKey == VK_SHIFT || rebind.fromKey == VK_LSHIFT || rebind.fromKey == VK_RSHIFT) {
                            ks[VK_SHIFT] = 0;
                            ks[VK_LSHIFT] = 0;
                            ks[VK_RSHIFT] = 0;
                        } else if (rebind.fromKey == VK_CONTROL || rebind.fromKey == VK_LCONTROL || rebind.fromKey == VK_RCONTROL) {
                            ks[VK_CONTROL] = 0;
                            ks[VK_LCONTROL] = 0;
                            ks[VK_RCONTROL] = 0;
                        } else if (rebind.fromKey == VK_MENU || rebind.fromKey == VK_LMENU || rebind.fromKey == VK_RMENU) {
                            ks[VK_MENU] = 0;
                            ks[VK_LMENU] = 0;
                            ks[VK_RMENU] = 0;
                        }

                        (void)TryTranslateVkToCharWithKeyboardState(textVK, ks, outChar);
                    }

                    if (outChar == 0) {
                        if (!TryTranslateVkToChar(textVK, false, outChar) || outChar == 0) {
                            (void)TryTranslateVkToChar(textVK, true, outChar);
                        }
                    }
                }

                if (outChar != 0) {
                    const UINT charMsg = isSystemKeyMsg ? WM_SYSCHAR : WM_CHAR;
                    if (charMsg == WM_CHAR) {
                        SendMessage(hWnd, WM_TOOLSCREEN_CHAR_NO_REBIND, static_cast<WPARAM>(outChar), charLParam);
                    } else {
                        CallWindowProc(g_originalWndProc, hWnd, charMsg, static_cast<WPARAM>(outChar), charLParam);
                    }
                }
            };

            // If the output is a modifier key, we must synthesize it via SendInput so the OS/game keyboard state
            if (isModifierVk(triggerVK)) {
                const bool sourceIsModifier = isModifierVk(rebind.fromKey) || isModifierVk(vkCode) || isModifierVk(rawVkCode);
                if (isAutoRepeatKeyDown && !sourceIsModifier) {
                    return { true, 0 };
                }

                (void)SendSynthKeyByScanCode(outputScanCode, isKeyDown);

                if (isKeyDown && fromKeyIsNonChar) {
                    const UINT textScanCode = GetScanCodeWithExtendedFlag(textVK);
                    LPARAM charLParam =
                        BuildKeyboardMessageLParam(textScanCode, true, isSystemKeyMsg, repeatCount, previousState, transitionState);
                    emitTypedChar(charLParam);
                }

                return { true, 0 };
            }

            // Windows typically sends generic modifier VKs in wParam (VK_SHIFT/VK_CONTROL/VK_MENU)
            const DWORD msgVk = [&]() -> DWORD {
                if (triggerVK == VK_LSHIFT || triggerVK == VK_RSHIFT) return VK_SHIFT;
                if (triggerVK == VK_LCONTROL || triggerVK == VK_RCONTROL) return VK_CONTROL;
                if (triggerVK == VK_LMENU || triggerVK == VK_RMENU) return VK_MENU;
                return triggerVK;
            }();

            LPARAM newLParam =
                BuildKeyboardMessageLParam(outputScanCode, isKeyDown, isSystemKeyMsg, repeatCount, previousState, transitionState);

            // Do NOT PostMessage keyboard outputs when the source event is a mouse button.
            LRESULT keyResult = CallWindowProc(g_originalWndProc, hWnd, outputMsg, msgVk, newLParam);

            if (isKeyDown && fromKeyIsNonChar) {
                emitTypedChar(newLParam);
            }

            return { true, keyResult };
        }
    }
    return { false, 0 };
}

InputHandlerResult HandleCustomKeyNoRebind(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg != WM_TOOLSCREEN_KEYDOWN_NO_REBIND && uMsg != WM_TOOLSCREEN_KEYUP_NO_REBIND) { return { false, 0 }; }
    PROFILE_SCOPE("HandleCustomKeyNoRebind");

    const UINT forwardedMsg = (uMsg == WM_TOOLSCREEN_KEYDOWN_NO_REBIND) ? WM_KEYDOWN : WM_KEYUP;

    if (g_showGui.load()) {
        ImGuiInputQueue_EnqueueWin32Message(hWnd, forwardedMsg, wParam, lParam);
        return { true, 1 };
    }

    if (g_originalWndProc) { return { true, CallWindowProc(g_originalWndProc, hWnd, forwardedMsg, wParam, lParam) }; }
    return { true, DefWindowProc(hWnd, forwardedMsg, wParam, lParam) };
}

InputHandlerResult HandleCustomCharNoRebind(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg != WM_TOOLSCREEN_CHAR_NO_REBIND) { return { false, 0 }; }
    PROFILE_SCOPE("HandleCustomCharNoRebind");

    HandleCharLogging(WM_CHAR, wParam, lParam);

    if (g_showGui.load()) {
        ImGuiInputQueue_EnqueueWin32Message(hWnd, WM_CHAR, wParam, lParam);
        return { true, 1 };
    }

    if (g_originalWndProc) { return { true, CallWindowProc(g_originalWndProc, hWnd, WM_CHAR, wParam, lParam) }; }
    return { true, DefWindowProc(hWnd, WM_CHAR, wParam, lParam) };
}

InputHandlerResult HandleCharRebinding(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg != WM_CHAR) { return { false, 0 }; }
    PROFILE_SCOPE("HandleCharRebinding");

    auto charRebindCfg = GetConfigSnapshot();
    if (!charRebindCfg || !charRebindCfg->keyRebinds.enabled) { return { false, 0 }; }

    WCHAR inputChar = static_cast<WCHAR>(wParam);

    for (const auto& rebind : charRebindCfg->keyRebinds.rebinds) {
        if (!rebind.enabled || rebind.fromKey == 0 || rebind.toKey == 0) continue;

        auto isModifierVk = [](DWORD vk) {
            return vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT ||
                   vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU || vk == VK_LWIN || vk == VK_RWIN;
        };

        WCHAR fromUnshifted = 0;
        WCHAR fromShifted = 0;
        bool hasFromUnshifted = TryTranslateVkToChar(rebind.fromKey, false, fromUnshifted);
        bool hasFromShifted = TryTranslateVkToChar(rebind.fromKey, true, fromShifted);

        bool matched = false;
        bool matchedShifted = false;

        if (hasFromUnshifted && inputChar == fromUnshifted) {
            matched = true;
            matchedShifted = false;
        } else if (hasFromShifted && inputChar == fromShifted) {
            matched = true;
            matchedShifted = true;
        }

        if (matched) {
            const DWORD baseOutVK = NormalizeModifierVkFromConfig(rebind.toKey, (rebind.useCustomOutput ? rebind.customOutputScanCode : 0));
            if (isModifierVk(baseOutVK)) {
                Log("[REBIND WM_CHAR] Consuming char code " + std::to_string(static_cast<unsigned int>(inputChar)) +
                    " (base output is modifier VK=" + std::to_string((unsigned int)baseOutVK) + ")");
                return { true, 0 };
            }

            if (rebind.useCustomOutput && rebind.customOutputUnicode != 0) {
                LRESULT r = SendUnicodeScalarAsCharMessage(hWnd, uMsg, (uint32_t)rebind.customOutputUnicode, lParam);
                return { true, r };
            }

            DWORD outputVK = (rebind.useCustomOutput && rebind.customOutputVK != 0) ? rebind.customOutputVK : rebind.toKey;
            outputVK = NormalizeModifierVkFromConfig(outputVK);

            WCHAR outputChar = 0;
            if (outputVK == VK_RETURN) {
                outputChar = L'\r';
            } else if (outputVK == VK_TAB) {
                outputChar = L'\t';
            } else if (outputVK == VK_BACK) {
                outputChar = L'\b';
            } else {
                if (!TryTranslateVkToChar(outputVK, matchedShifted, outputChar) || outputChar == 0) {
                    (void)TryTranslateVkToChar(outputVK, false, outputChar);
                }
            }

            if (outputChar == 0) {
                Log("[REBIND WM_CHAR] Consuming char code " + std::to_string(static_cast<unsigned int>(inputChar)) +
                    " (output VK has no WM_CHAR)");
                return { true, 0 };
            }

            Log("[REBIND WM_CHAR] Remapping char code " + std::to_string(static_cast<unsigned int>(inputChar)) + " -> " +
                std::to_string(static_cast<unsigned int>(outputChar)));

            return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, outputChar, lParam) };
        }
    }
    return { false, 0 };
}

LRESULT CALLBACK SubclassedWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    PROFILE_SCOPE("SubclassedWndProc");

    const HWND expectedHwnd = g_subclassedHwnd.load();
    if (expectedHwnd != NULL && hWnd != expectedHwnd) {
        return DefWindowProc(hWnd, uMsg, wParam, lParam);
    }

    if (g_showGui.load() && s_forcedShowCursor && g_gameVersion >= GameVersion(1, 13, 0)) {
        EnsureSystemCursorVisible();
        static HCURSOR s_arrowCursor = LoadCursorW(NULL, IDC_ARROW);
        SetCursor(s_arrowCursor);
    }
    if (!g_showGui.load() && s_forcedShowCursor) {
        EnsureSystemCursorHidden();
        s_forcedShowCursor = false;
    }

    RegisterBindingInputEvent(uMsg, wParam, lParam);

    // This must run even in windowed mode (HandleNonFullscreenCheck returns early).
    switch (uMsg) {
    case WM_MOVE:
    case WM_MOVING:
        InvalidateCachedScreenMetrics();
        break;
    case WM_SIZE:
    case WM_SIZING:
    case WM_WINDOWPOSCHANGED:
    case WM_DPICHANGED:
    case WM_DISPLAYCHANGE:
        RequestScreenMetricsRecalculation();
        InvalidateImGuiCache();
        // Force GUI to recenter after window resize so it renders at correct position
        g_guiNeedsRecenter = true;
        break;
    default:
        break;
    }

    InputHandlerResult result;

    result = HandleMouseMoveViewportOffset(hWnd, uMsg, wParam, lParam);

    result = HandleShutdownCheck(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    result = HandleWindowValidation(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    result = HandleToolscreenQueryMessages(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    result = HandleBorderlessToggle(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    result = HandleImageOverlaysToggle(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;
    result = HandleWindowOverlaysToggle(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    result = HandleNonFullscreenCheck(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    HandleCharLogging(uMsg, wParam, lParam);

    result = HandleWindowPosChanged(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    result = HandleAltF4(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    result = HandleConfigLoadFailure(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    std::string currentModeId = g_modeIdBuffers[g_currentModeIdIndex.load(std::memory_order_acquire)];
    std::string localGameState = g_gameStateBuffers[g_currentGameStateIndex.load(std::memory_order_acquire)];

    result = HandleSetCursor(hWnd, uMsg, wParam, lParam, localGameState);
    if (result.consumed) return result.result;

    result = HandleDestroy(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    if (g_isShuttingDown.load()) { return CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam); }

    result = HandleImGuiInput(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    result = HandleGuiToggle(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    result = HandleWindowOverlayKeyboard(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    result = HandleWindowOverlayMouse(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    result = HandleGuiInputBlocking(uMsg);
    if (result.consumed) return result.result;

    result = HandleActivate(hWnd, uMsg, wParam, lParam, currentModeId);
    if (result.consumed) return result.result;

    result = HandleWmSizeModeDimensions(hWnd, uMsg, wParam, lParam, currentModeId);
    if (result.consumed) return result.result;

    result = HandleHotkeys(hWnd, uMsg, wParam, lParam, currentModeId, localGameState);
    if (result.consumed) return result.result;

    result = HandleMouseCoordinateTranslationPhase(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    result = HandleCustomKeyNoRebind(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    result = HandleKeyRebinding(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    result = HandleCustomCharNoRebind(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    result = HandleCharRebinding(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    return CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam);
}


