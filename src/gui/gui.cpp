#include "gui.h"
#include "gui_internal.h"
#include "config/config_toml.h"
#include "common/expression_parser.h"
#include "features/fake_cursor.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_win32.h"
#include "imgui_stdlib.h"
#include "runtime/logic_thread.h"
#include "render/mirror_thread.h"
#include "common/profiler.h"
#include "render/render.h"
#include "render/render_thread.h"
#include "platform/resource.h"
#include <nlohmann/json.hpp>
#include "common/i18n.h"
#include "third_party/stb_image.h"
#include "common/utils.h"
#include "features/virtual_camera.h"
#include "features/window_overlay.h"

#include <GL/glew.h>
#include <ShlObj.h>
#include <shellapi.h>
#include <Shlwapi.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <commdlg.h>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <set>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <winhttp.h>
#include <windowsx.h>

#pragma comment(lib, "Shlwapi.lib")
#pragma comment(lib, "Winhttp.lib")

static constexpr const wchar_t* DISCORD_URL = L"https://discord.gg/A2v6bCJg6K";

void ApplyKeyRepeatSettings();

extern std::atomic<bool> g_gameWindowActive;

static constexpr float spinnerHoldDelay = 0.2f;
static constexpr float spinnerHoldInterval = 0.01f;

ImFont* g_keyboardLayoutPrimaryFont = nullptr;
ImFont* g_keyboardLayoutSecondaryFont = nullptr;

static std::atomic<bool> g_wasCursorVisible{ true };

#define SliderFloat SliderFloatDoubleClickInput
#define SliderInt SliderIntDoubleClickInput

// This function MUST be defined before the JSON serialization functions that call it
EyeZoomConfig GetDefaultEyeZoomConfig() { return GetDefaultEyeZoomConfigFromEmbedded(); }

void RenderConfigErrorGUI() {
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 center = ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(600, 0));
    if (ImGui::Begin(trc("error.configuration_error"), NULL,
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove)) {
        static std::chrono::steady_clock::time_point s_lastCopyTime{};
        std::string errorMsg;
        {
            std::lock_guard<std::mutex> lock(g_configErrorMutex);
            errorMsg = g_configLoadError;
        }
        ImGui::TextWrapped("A critical error occurred while loading the configuration file (config.toml).");
        ImGui::Separator();
        ImGui::TextWrapped("%s", errorMsg.c_str());
        ImGui::Separator();
        ImGui::TextWrapped("The application cannot continue. To get help, copy the debug info and send it to a "
                           "developer. Otherwise, please quit the game.");
        ImGui::Separator();

        bool show_feedback =
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - s_lastCopyTime).count() < 3;

        float button_width_copy = ImGui::CalcTextSize(trc("button.copy_debug_info")).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        float button_width_quit = ImGui::CalcTextSize(trc("button.quit")).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        float total_button_width = button_width_copy + button_width_quit + ImGui::GetStyle().ItemSpacing.x;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - total_button_width) * 0.5f);

        if (ImGui::Button(trc("button.copy_debug_info"))) {
            std::string configContent = "ERROR: Could not read config.toml.";
            std::wstring configPath = g_toolscreenPath + L"\\config.toml";
            std::ifstream f(std::filesystem::path(configPath), std::ios::binary);
            if (f.is_open()) {
                std::ostringstream ss;
                ss << f.rdbuf();
                configContent = ss.str();
                f.close();
            }

            std::string fullDebugInfo = "Error Message:\r\n";
            fullDebugInfo += "----------------------------------------\r\n";
            fullDebugInfo += errorMsg;
            fullDebugInfo += "\r\n\r\n\r\nRaw config.toml Content:\r\n";
            fullDebugInfo += "----------------------------------------\r\n";
            fullDebugInfo += configContent;

            CopyToClipboard(g_minecraftHwnd.load(), fullDebugInfo);
            s_lastCopyTime = std::chrono::steady_clock::now();
        }

        ImGui::SameLine();
        if (ImGui::Button(trc("button.quit"))) { exit(0); }

        if (show_feedback) {
            const char* feedback_text = "Debug info copied to clipboard!";
            float feedback_width = ImGui::CalcTextSize(feedback_text).x;
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - feedback_width) * 0.5f);
            ImGui::TextUnformatted(feedback_text);
        }

        ImGui::End();
    }
}

void RenderSettingsGUI() {
    PROFILE_SCOPE_CAT("Settings GUI Rendering", "ImGui");
    ResetTransientBindingUiState();

    static const std::vector<std::pair<const char*, const char*>>
        relativeToOptions = {
            {"topLeftViewport", trc("position.top_left_viewport")},
            {"topRightViewport", trc("position.top_right_viewport")},
            {"bottomLeftViewport", trc("position.bottom_left_viewport")},
            {"bottomRightViewport", trc("position.bottom_right_viewport")},
            {"centerViewport", trc("position.center_viewport")},
            {"pieLeft", trc("position.pie_left")},
            {"pieRight", trc("position.pie_right")},
            {"topLeftScreen", trc("position.top_left_screen")},
            {"topRightScreen", trc("position.top_right_screen")},
            {"bottomLeftScreen", trc("position.bottom_left_screen")},
            {"bottomRightScreen", trc("position.bottom_right_screen")},
            {"centerScreen", trc("position.center_screen")}
        };
    static const std::vector<std::pair<const char*, const char*>>
        imageRelativeToOptions = {
            {"topLeftViewport", trc("position.top_left_viewport")},
            {"topRightViewport", trc("position.top_right_viewport")},
            {"bottomLeftViewport", trc("position.bottom_left_viewport")},
            {"bottomRightViewport", trc("position.bottom_right_viewport")},
            {"centerViewport", trc("position.center_viewport")},
            {"topLeftScreen", trc("position.top_left_screen")},
            {"topRightScreen", trc("position.top_right_screen")},
            {"bottomLeftScreen", trc("position.bottom_left_screen")},
            {"bottomRightScreen", trc("position.bottom_right_screen")},
            {"centerScreen", trc("position.center_screen")}
        };
    auto getFriendlyName = [&](const std::string& key, const std::vector<std::pair<const char*, const char*>>& options) {
        for (const auto& option : options) {
            if (key == option.first) return option.second;
        }
        return "Unknown";
    };

    static std::vector<DWORD> s_bindingKeys;
    static bool s_hadKeysPressed = false;
    static std::set<DWORD> s_preHeldKeys;
    static bool s_bindingInitialized = false;

    static const std::vector<const char*> validGameStates = { "wall",    "inworld,cursor_free", "inworld,cursor_grabbed", "title",
                                                              "waiting", "generating" };

    static const std::vector<const char*> guiGameStates = { "wall", "inworld,cursor_free", "inworld,cursor_grabbed", "title",
                                                            "generating" };

    static const std::vector<std::pair<const char*, const char*>>
        gameStateDisplayNames = {
            {"wall", trc("game_state.wall")},
            {"inworld,cursor_free", trc("game_state.inworld_free")},
            {"inworld,cursor_grabbed", trc("game_state.inworld_grabbed")},
            {"title", trc("game_state.title")},
            {"waiting", trc("game_state.waiting")},
            {"generating", trc("game_state.generating")}
        };

    auto getGameStateFriendlyName = [&](const std::string& gameState) {
        for (const auto& pair : gameStateDisplayNames) {
            if (gameState == pair.first) return pair.second;
        }
        return gameState.c_str();
    };

    bool is_binding = IsHotkeyBindingActive_UiState();
    if (is_binding) { MarkHotkeyBindingActive(); }

    if (is_binding) {
        if (!s_bindingInitialized) {
            s_preHeldKeys.clear();
            for (int vk = 1; vk < 0xFF; ++vk) {
                if (GetAsyncKeyState(vk) & 0x8000) {
                    s_preHeldKeys.insert(static_cast<DWORD>(vk));
                }
            }
            s_bindingInitialized = true;
        }
        ImGui::OpenPopup(trc("hotkeys.bind_hotkey"));
    } else {
        s_bindingKeys.clear();
        s_hadKeysPressed = false;
        s_preHeldKeys.clear();
        s_bindingInitialized = false;
    }

    if (ImGui::BeginPopupModal(trc("hotkeys.bind_hotkey"), NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::Text(trc("hotkeys.bind_hotkey.tooltip.prompt"));
        ImGui::Text(trc("hotkeys.bind_hotkey.tooltip.confirm"));
        ImGui::Text(trc("hotkeys.bind_hotkey.tooltip.clear"));
        ImGui::Text(trc("hotkeys.bind_hotkey.tooltip.cancel"));
        ImGui::Separator();

        static uint64_t s_lastBindingInputSeqHotkeyBind = 0;
        if (ImGui::IsWindowAppearing()) { s_lastBindingInputSeqHotkeyBind = GetLatestBindingInputSequence(); }

        static std::string s_hotkeyConflictMessage;

        auto finalize_bind = [&](const std::vector<DWORD>& keys) {
            if (!keys.empty() && s_exclusionToBind.hotkey_idx == -1) {
                std::string excludeLabel;
                if (s_mainHotkeyToBind == -999) excludeLabel = "GUI Toggle";
                else if (s_mainHotkeyToBind == -998) excludeLabel = "Borderless Toggle";
                else if (s_mainHotkeyToBind == -997) excludeLabel = "Image Overlays Toggle";
                else if (s_mainHotkeyToBind == -996) excludeLabel = "Window Overlays Toggle";
                else if (s_mainHotkeyToBind == -995) excludeLabel = "Key Rebinds Toggle";
                else if (s_mainHotkeyToBind >= 0) excludeLabel = "Mode Hotkey #" + std::to_string(s_mainHotkeyToBind + 1);
                else if (s_sensHotkeyToBind != -1) excludeLabel = "Sensitivity Hotkey #" + std::to_string(s_sensHotkeyToBind + 1);
                else if (s_altHotkeyToBind.hotkey_idx != -1) excludeLabel = "Mode Hotkey #" + std::to_string(s_altHotkeyToBind.hotkey_idx + 1) + " Alt #" + std::to_string(s_altHotkeyToBind.alt_idx + 1);

                std::string conflict = FindHotkeyConflict(keys, excludeLabel);
                if (!conflict.empty()) {
                    s_hotkeyConflictMessage = "Already assigned to " + conflict;
                    s_bindingKeys.clear();
                    s_hadKeysPressed = false;
                    return;
                }
            }
            s_hotkeyConflictMessage.clear();

            if (s_mainHotkeyToBind != -1) {
                if (s_mainHotkeyToBind == -999) {
                    g_config.guiHotkey = keys;
                } else if (s_mainHotkeyToBind == -998) {
                    g_config.borderlessHotkey = keys;
                } else if (s_mainHotkeyToBind == -997) {
                    g_config.imageOverlaysHotkey = keys;
                } else if (s_mainHotkeyToBind == -996) {
                    g_config.windowOverlaysHotkey = keys;
                } else if (s_mainHotkeyToBind == -995) {
                    g_config.keyRebinds.toggleHotkey = keys;
                } else {
                    g_config.hotkeys[s_mainHotkeyToBind].keys = keys;
                }
                s_mainHotkeyToBind = -1;
            } else if (s_sensHotkeyToBind != -1) {
                g_config.sensitivityHotkeys[s_sensHotkeyToBind].keys = keys;
                s_sensHotkeyToBind = -1;
            } else if (s_altHotkeyToBind.hotkey_idx != -1) {
                g_config.hotkeys[s_altHotkeyToBind.hotkey_idx].altSecondaryModes[s_altHotkeyToBind.alt_idx].keys = keys;
                s_altHotkeyToBind = { -1, -1 };
            } else if (s_exclusionToBind.hotkey_idx != -1) {
                if (!keys.empty()) {
                    g_config.hotkeys[s_exclusionToBind.hotkey_idx].conditions.exclusions[s_exclusionToBind.exclusion_idx] = keys.back();
                }
                s_exclusionToBind = { -1, -1 };
            }
            g_configIsDirty = true;

            {
                std::lock_guard<std::mutex> hotkeyLock(g_hotkeyMainKeysMutex);
                RebuildHotkeyMainKeys_Internal();
            }

            s_bindingKeys.clear();
            s_hadKeysPressed = false;
            s_preHeldKeys.clear();
            s_bindingInitialized = false;
            ImGui::CloseCurrentPopup();
        };

        DWORD capturedVk = 0;
        LPARAM capturedLParam = 0;
        bool capturedIsMouse = false;
        if (ConsumeBindingInputEventSince(s_lastBindingInputSeqHotkeyBind, capturedVk, capturedLParam, capturedIsMouse)) {
            if (capturedVk == VK_ESCAPE) {
                Log("Binding cancelled from Escape key.");
                s_mainHotkeyToBind = -1;
                s_sensHotkeyToBind = -1;
                s_exclusionToBind = { -1, -1 };
                s_altHotkeyToBind = { -1, -1 };
                s_bindingKeys.clear();
                s_hadKeysPressed = false;
                s_preHeldKeys.clear();
                s_bindingInitialized = false;
                ImGui::CloseCurrentPopup();
                (void)capturedLParam;
                (void)capturedIsMouse;
                ImGui::EndPopup();
                return;
            }

            const bool canClear = (s_exclusionToBind.hotkey_idx == -1);
            if (canClear && (capturedVk == VK_BACK || capturedVk == VK_DELETE)) {
                Log("Binding cleared from Backspace/Delete.");
                finalize_bind({});
                ImGui::EndPopup();
                return;
            }
        }

        {
            const bool canClear = (s_exclusionToBind.hotkey_idx == -1);
            if (!canClear) { ImGui::BeginDisabled(); }
            if (ImGui::Button(trc("button.clear"))) {
                finalize_bind({});
                ImGui::EndPopup();
                return;
            }
            if (!canClear) { ImGui::EndDisabled(); }

            ImGui::SameLine();
            if (ImGui::Button(trc("button.cancel"))) {
                Log("Binding cancelled from Cancel button.");
                s_mainHotkeyToBind = -1;
                s_sensHotkeyToBind = -1;
                s_exclusionToBind = { -1, -1 };
                s_altHotkeyToBind = { -1, -1 };
                s_bindingKeys.clear();
                s_hadKeysPressed = false;
                s_preHeldKeys.clear();
                s_bindingInitialized = false;
                ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
                return;
            }
            ImGui::Separator();
        }

        // Evict pre-held keys once they are physically released
        for (auto it = s_preHeldKeys.begin(); it != s_preHeldKeys.end(); ) {
            if (!(GetAsyncKeyState(*it) & 0x8000)) {
                it = s_preHeldKeys.erase(it);
            } else {
                ++it;
            }
        }

        std::vector<DWORD> currentlyPressed;

        const bool lctrlDown = (GetAsyncKeyState(VK_LCONTROL) & 0x8000) != 0;
        const bool rctrlDown = (GetAsyncKeyState(VK_RCONTROL) & 0x8000) != 0;
        const bool lshiftDown = (GetAsyncKeyState(VK_LSHIFT) & 0x8000) != 0;
        const bool rshiftDown = (GetAsyncKeyState(VK_RSHIFT) & 0x8000) != 0;
        const bool laltDown = (GetAsyncKeyState(VK_LMENU) & 0x8000) != 0;
        const bool raltDown = (GetAsyncKeyState(VK_RMENU) & 0x8000) != 0;

        const bool lctrlPreHeld = s_preHeldKeys.count(VK_LCONTROL) || s_preHeldKeys.count(VK_CONTROL);
        const bool rctrlPreHeld = s_preHeldKeys.count(VK_RCONTROL) || s_preHeldKeys.count(VK_CONTROL);
        const bool lshiftPreHeld = s_preHeldKeys.count(VK_LSHIFT) || s_preHeldKeys.count(VK_SHIFT);
        const bool rshiftPreHeld = s_preHeldKeys.count(VK_RSHIFT) || s_preHeldKeys.count(VK_SHIFT);
        const bool laltPreHeld = s_preHeldKeys.count(VK_LMENU) || s_preHeldKeys.count(VK_MENU);
        const bool raltPreHeld = s_preHeldKeys.count(VK_RMENU) || s_preHeldKeys.count(VK_MENU);

        if (lctrlDown && !lctrlPreHeld) currentlyPressed.push_back(VK_LCONTROL);
        if (rctrlDown && !rctrlPreHeld) currentlyPressed.push_back(VK_RCONTROL);
        if (lshiftDown && !lshiftPreHeld) currentlyPressed.push_back(VK_LSHIFT);
        if (rshiftDown && !rshiftPreHeld) currentlyPressed.push_back(VK_RSHIFT);
        if (laltDown && !laltPreHeld) currentlyPressed.push_back(VK_LMENU);
        if (raltDown && !raltPreHeld) currentlyPressed.push_back(VK_RMENU);

        for (int vk = 1; vk < 0xFF; ++vk) {
            // Skip escape (used for cancel), generic modifiers, and Windows keys
            if (vk == VK_ESCAPE || vk == VK_LBUTTON || vk == VK_CONTROL || vk == VK_SHIFT || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN ||
                vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_LMENU || vk == VK_RMENU) {
                continue;
            }
            if (s_preHeldKeys.count(static_cast<DWORD>(vk))) continue;
            if (GetAsyncKeyState(vk) & 0x8000) { currentlyPressed.push_back(vk); }
        }

        for (DWORD key : currentlyPressed) {
            if (std::find(s_bindingKeys.begin(), s_bindingKeys.end(), key) == s_bindingKeys.end()) {
                bool isModifier = (key == VK_CONTROL || key == VK_SHIFT || key == VK_MENU || key == VK_LCONTROL || key == VK_RCONTROL ||
                                   key == VK_LSHIFT || key == VK_RSHIFT || key == VK_LMENU || key == VK_RMENU);
                if (isModifier) {
                    auto insertPos = s_bindingKeys.begin();
                    for (auto it = s_bindingKeys.begin(); it != s_bindingKeys.end(); ++it) {
                        bool itIsModifier = (*it == VK_CONTROL || *it == VK_SHIFT || *it == VK_MENU || *it == VK_LCONTROL || *it == VK_RCONTROL ||
                                             *it == VK_LSHIFT || *it == VK_RSHIFT || *it == VK_LMENU || *it == VK_RMENU);
                        if (!itIsModifier) {
                            insertPos = it;
                            break;
                        }
                        insertPos = it + 1;
                    }
                    s_bindingKeys.insert(insertPos, key);
                } else {
                    s_bindingKeys.push_back(key);
                }
            }
        }

        if (!currentlyPressed.empty()) {
            if (!s_hadKeysPressed) s_hotkeyConflictMessage.clear();
            s_hadKeysPressed = true;
        }

        if (s_hadKeysPressed && currentlyPressed.empty()) {
            finalize_bind(s_bindingKeys);
            if (s_hotkeyConflictMessage.empty()) {
                ImGui::EndPopup();
                return;
            }
        }

        if (!s_bindingKeys.empty()) {
            std::string combo = GetKeyComboString(s_bindingKeys);
            ImGui::Text(tr("hotkeys.bind_hotkey.current", combo.c_str()).c_str());
        } else {
            ImGui::Text(trc("hotkeys.bind_hotkey.current_none"));
        }

        if (!s_hotkeyConflictMessage.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", s_hotkeyConflictMessage.c_str());

        ImGui::EndPopup();
    }

    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowSizeConstraints(ImVec2(500, 400), ImVec2(FLT_MAX, FLT_MAX));

    const int screenWidth = GetCachedWindowWidth();
    const int screenHeight = GetCachedWindowHeight();

    const float scaleFactor = ComputeGuiScaleFactorFromCachedWindowSize();
    static float s_lastRuntimeScaleFactor = -1.0f;
    if (s_lastRuntimeScaleFactor < 0.0f || fabsf(scaleFactor - s_lastRuntimeScaleFactor) > 0.001f) {
        g_guiNeedsRecenter.store(true, std::memory_order_relaxed);
        s_lastRuntimeScaleFactor = scaleFactor;
    }

    static ImVec2 s_lastDisplaySize = ImVec2(-1.0f, -1.0f);
    if (io.DisplaySize.x > 0.0f && io.DisplaySize.y > 0.0f) {
        const bool displaySizeChanged =
            fabsf(io.DisplaySize.x - s_lastDisplaySize.x) > 0.5f || fabsf(io.DisplaySize.y - s_lastDisplaySize.y) > 0.5f;
        if (displaySizeChanged) { g_guiNeedsRecenter.store(true, std::memory_order_relaxed); }
        s_lastDisplaySize = io.DisplaySize;
    }

    if (g_guiNeedsRecenter.exchange(false)) {
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(850 * scaleFactor, 650 * scaleFactor), ImGuiCond_Always);
    }

    std::string windowTitle = "Toolscreen v" + GetToolscreenVersionString() + " by jojoe77777";

    bool windowOpen = true;
    if (ImGui::Begin(windowTitle.c_str(), &windowOpen, ImGuiWindowFlags_NoCollapse)) {
        if (!windowOpen) {
            g_showGui = false;
            if (!g_wasCursorVisible.load()) {
                RECT clipRect{};
                HWND hwnd = g_minecraftHwnd.load(std::memory_order_relaxed);
                if (GetWindowClientRectInScreen(hwnd, clipRect)) {
                    ClipCursor(&clipRect);
                } else {
                    ClipCursor(NULL);
                }
                SetCursor(NULL);
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
        }

        {
            static std::chrono::steady_clock::time_point s_lastScreenshotTime{};
            auto now = std::chrono::steady_clock::now();
            bool showCopied = std::chrono::duration_cast<std::chrono::seconds>(now - s_lastScreenshotTime).count() < 3;

            const char* buttonLabel = showCopied ? trc("button.screenshot.copied") : trc("button.screenshot");
            float buttonWidth = ImGui::CalcTextSize(buttonLabel).x + ImGui::GetStyle().FramePadding.x * 2.0f;

            ImVec2 savedCursor = ImGui::GetCursorPos();

            {
                static GLuint s_languageTexture = 0;
                static HGLRC s_languageLastCtx = NULL;
                HGLRC currentCtx = wglGetCurrentContext();
                if (currentCtx != s_languageLastCtx) {
                    s_languageTexture = 0;
                    s_languageLastCtx = currentCtx;
                }

                auto ensureLanguageTextureLoaded = [&]() {
                    if (s_languageTexture != 0) return;

                    HMODULE hModule = NULL;
                    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       (LPCWSTR)&g_showGui, &hModule);
                    if (!hModule) return;

                    HRSRC hResource = FindResourceW(hModule, MAKEINTRESOURCEW(IDR_LANGUAGE_PNG), RT_RCDATA);
                    if (!hResource) return;

                    HGLOBAL hData = LoadResource(hModule, hResource);
                    if (!hData) return;

                    DWORD dataSize = SizeofResource(hModule, hResource);
                    const unsigned char* rawData = (const unsigned char*)LockResource(hData);
                    if (!rawData || dataSize == 0) return;

                    stbi_set_flip_vertically_on_load_thread(0);
                    int w = 0, h = 0, channels = 0;
                    unsigned char* pixels = stbi_load_from_memory(rawData, (int)dataSize, &w, &h, &channels, 4);
                    if (!pixels || w <= 0 || h <= 0) {
                        if (pixels) stbi_image_free(pixels);
                        return;
                    }

                    glGenTextures(1, &s_languageTexture);
                    glBindTexture(GL_TEXTURE_2D, s_languageTexture);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
                    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
                    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
                    glBindTexture(GL_TEXTURE_2D, 0);
                    stbi_image_free(pixels);
                };

                ensureLanguageTextureLoaded();

                if (s_languageTexture != 0) {
                    float iconSize = ImGui::GetFrameHeight();
                    float margin = ImGui::GetStyle().ItemSpacing.x;
                    ImGui::SetCursorPos(ImVec2(ImGui::GetWindowContentRegionMax().x - buttonWidth - iconSize * 2 - margin * 2, 30.0f));
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.1f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.2f));
                    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
                    if (ImGui::ImageButton("##language", (ImTextureID)(intptr_t)s_languageTexture, ImVec2(iconSize, iconSize))) {
                        ImGui::OpenPopup("##LanguagePopup");
                    }
                    ImGui::PopStyleVar();
                    ImGui::PopStyleColor(3);

                    if (ImGui::BeginPopup("##LanguagePopup")) {
                        nlohmann::json langs = GetLangs();
                        for (const auto& [langCode, langName] : langs.items()) {
                            Log(langName.get<std::string>());
                            bool isSelected = (g_config.lang == langCode);
                            if (ImGui::Selectable(langName.get<std::string>().c_str(), isSelected)) {
                                if (g_config.lang != langCode) {
                                    g_config.lang = langCode;
                                    LoadTranslation(langCode);
                                    g_configIsDirty = true;
                                }
                            }
                            if (isSelected) {
                                ImGui::SetItemDefaultFocus();
                            }
                        }
                        ImGui::EndPopup(); 
                    }
                }
            }

            {
                static GLuint s_discordTexture = 0;
                static HGLRC s_discordLastCtx = NULL;
                HGLRC currentCtx = wglGetCurrentContext();
                if (currentCtx != s_discordLastCtx) {
                    s_discordTexture = 0;
                    s_discordLastCtx = currentCtx;
                }

                auto ensureDiscordTextureLoaded = [&]() {
                    if (s_discordTexture != 0) return;

                    HMODULE hModule = NULL;
                    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       (LPCWSTR)&g_showGui, &hModule);
                    if (!hModule) return;

                    HRSRC hResource = FindResourceW(hModule, MAKEINTRESOURCEW(IDR_DISCORD_PNG), RT_RCDATA);
                    if (!hResource) return;

                    HGLOBAL hData = LoadResource(hModule, hResource);
                    if (!hData) return;

                    DWORD dataSize = SizeofResource(hModule, hResource);
                    const unsigned char* rawData = (const unsigned char*)LockResource(hData);
                    if (!rawData || dataSize == 0) return;

                    stbi_set_flip_vertically_on_load_thread(0);
                    int w = 0, h = 0, channels = 0;
                    unsigned char* pixels = stbi_load_from_memory(rawData, (int)dataSize, &w, &h, &channels, 4);
                    if (!pixels || w <= 0 || h <= 0) {
                        if (pixels) stbi_image_free(pixels);
                        return;
                    }

                    glGenTextures(1, &s_discordTexture);
                    BindTextureDirect(GL_TEXTURE_2D, s_discordTexture);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
                    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
                    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
                    BindTextureDirect(GL_TEXTURE_2D, 0);
                    stbi_image_free(pixels);
                };

                ensureDiscordTextureLoaded();

                if (s_discordTexture != 0) {
                    float iconSize = ImGui::GetFrameHeight();
                    float margin = ImGui::GetStyle().ItemSpacing.x;
                    ImGui::SetCursorPos(ImVec2(ImGui::GetWindowContentRegionMax().x - buttonWidth - iconSize - margin, 30.0f));
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.1f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.2f));
                    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
                    if (ImGui::ImageButton("##discord", (ImTextureID)(intptr_t)s_discordTexture, ImVec2(iconSize, iconSize))) {
                        ShellExecuteW(NULL, L"open", DISCORD_URL, NULL, NULL, SW_SHOWNORMAL);
                    }
                    ImGui::PopStyleVar();
                    ImGui::PopStyleColor(3);
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip(trc("tooltip.join_discord"));
                    }
                }
            }

            ImGui::SetCursorPos(ImVec2(ImGui::GetWindowContentRegionMax().x - buttonWidth, 30.0f));

            if (ImGui::Button(buttonLabel)) {
                g_screenshotRequested = true;
                s_lastScreenshotTime = std::chrono::steady_clock::now();
            }

            ImGui::SetCursorPos(savedCursor);
        }

        {
            bool isAdvanced = !g_config.basicModeEnabled;
            if (ImGui::RadioButton(trc("config_mode.basic"), !isAdvanced)) {
                g_config.basicModeEnabled = true;
                g_configIsDirty = true;
            }
            ImGui::SameLine();
            if (ImGui::RadioButton(trc("config_mode.advanced"), isAdvanced)) {
                g_config.basicModeEnabled = false;
                g_configIsDirty = true;
            }
        }

        ImGui::Separator();

        // Drag modes must only be enabled by the currently active tab.
        g_imageDragMode.store(false, std::memory_order_relaxed);
        g_windowOverlayDragMode.store(false, std::memory_order_relaxed);

        if (g_config.basicModeEnabled) {
            if (ImGui::BeginTabBar("BasicSettingsTabs")) {
#include "tabs/tab_basic_general.inl"
#include "tabs/tab_basic_other.inl"

#include "tabs/tab_supporters.inl"

                ImGui::EndTabBar();
            }
        } else {
            if (ImGui::BeginTabBar("SettingsTabs")) {
#include "tabs/tab_modes.inl"
#include "tabs/tab_mirrors.inl"
#include "tabs/tab_images.inl"
#include "tabs/tab_window_overlays.inl"
#include "tabs/tab_hotkeys.inl"
#include "tabs/tab_inputs.inl"
#include "tabs/tab_settings.inl"

#include "tabs/tab_appearance.inl"

#include "tabs/tab_misc.inl"

#include "tabs/tab_supporters.inl"

                ImGui::EndTabBar();
            }
        }

    } else {
        g_currentlyEditingMirror = "";
        g_imageDragMode.store(false, std::memory_order_relaxed);
        g_windowOverlayDragMode.store(false, std::memory_order_relaxed);
    }
    ImGui::End();

    SaveConfig();

    // Ensure config snapshot is published for reader threads after GUI mutations.
    // update to prevent reader threads from seeing stale/freed vector data.
    if (g_configIsDirty.load()) { PublishConfigSnapshot(); }
}

