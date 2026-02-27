if (ImGui::BeginTabItem("General")) {
    g_currentlyEditingMirror = "";
    g_imageDragMode.store(false);
    g_windowOverlayDragMode.store(false);

    SliderCtrlClickTip();

    ImGui::SeparatorText("Window");
    {
        HWND hwnd = g_minecraftHwnd.load(std::memory_order_relaxed);
        const bool canToggleBorderless = (hwnd != NULL && IsWindow(hwnd));

        if (!canToggleBorderless) { ImGui::BeginDisabled(); }
        if (ImGui::Button("Go Borderless", ImVec2(150, 0))) {
            ToggleBorderlessWindowedFullscreen(hwnd);
        }
        if (!canToggleBorderless) { ImGui::EndDisabled(); }

        ImGui::SameLine();
        HelpMarker("Instantly toggles the game window into borderless mode.\n"
                   "If already borderless, it returns to windowed mode.");
    }

    auto RenderInlineHotkeyBinding = [&](const std::string& targetModeId, const char* label) {
        int hotkeyIdx = -1;
        for (size_t i = 0; i < g_config.hotkeys.size(); ++i) {
            if (EqualsIgnoreCase(g_config.hotkeys[i].mainMode, "Fullscreen") &&
                EqualsIgnoreCase(g_config.hotkeys[i].secondaryMode, targetModeId)) {
                hotkeyIdx = static_cast<int>(i);
                break;
            }
        }

        ImGui::SameLine();
        ImGui::Text("Hotkey:");
        ImGui::SameLine();

        if (hotkeyIdx != -1) {
            std::string keyStr = GetKeyComboString(g_config.hotkeys[hotkeyIdx].keys);
            bool isBinding = (s_mainHotkeyToBind == hotkeyIdx);
            const char* buttonLabel = isBinding ? "[Press Keys...]" : (keyStr.empty() ? "[Click to Bind]" : keyStr.c_str());

            ImGui::PushID(label);
            if (ImGui::Button(buttonLabel, ImVec2(120, 0))) {
                s_mainHotkeyToBind = hotkeyIdx;
                s_altHotkeyToBind = { -1, -1 };
                s_exclusionToBind = { -1, -1 };
                MarkHotkeyBindingActive();
            }
            ImGui::PopID();
        } else {
            ImGui::TextDisabled("[No hotkey]");
        }
    };

    auto EnsureModeExists = [&](const std::string& modeId, int width, int height) {
        for (const auto& mode : g_config.modes) {
            if (EqualsIgnoreCase(mode.id, modeId)) return;
        }
        ModeConfig newMode;
        newMode.id = modeId;
        newMode.width = width;
        newMode.height = height;
        newMode.background.selectedMode = "color";
        newMode.background.color = { 0.0f, 0.0f, 0.0f };
        g_config.modes.push_back(newMode);
        g_configIsDirty = true;
    };

    auto EnsureHotkeyForMode = [&](const std::string& targetModeId) {
        for (const auto& hotkey : g_config.hotkeys) {
            if (EqualsIgnoreCase(hotkey.mainMode, "Fullscreen") && EqualsIgnoreCase(hotkey.secondaryMode, targetModeId)) {
                return;
            }
        }
        HotkeyConfig newHotkey;
        newHotkey.keys = std::vector<DWORD>();
        newHotkey.mainMode = "Fullscreen";
        newHotkey.secondaryMode = targetModeId;
        newHotkey.debounce = 100;
        g_config.hotkeys.push_back(newHotkey);
        ResizeHotkeySecondaryModes(g_config.hotkeys.size());
        SetHotkeySecondaryMode(g_config.hotkeys.size() - 1, targetModeId);
        std::lock_guard<std::mutex> hotkeyLock(g_hotkeyMainKeysMutex);
        RebuildHotkeyMainKeys_Internal();
        g_configIsDirty = true;
    };

    auto RemoveModeAndHotkey = [&](const std::string& modeId) {
        for (auto it = g_config.modes.begin(); it != g_config.modes.end(); ++it) {
            if (EqualsIgnoreCase(it->id, modeId)) {
                g_config.modes.erase(it);
                break;
            }
        }
        g_config.hotkeys.erase(std::remove_if(g_config.hotkeys.begin(), g_config.hotkeys.end(),
                                              [&](const HotkeyConfig& h) { return EqualsIgnoreCase(h.secondaryMode, modeId); }),
                               g_config.hotkeys.end());
        ResetAllHotkeySecondaryModes();
        std::lock_guard<std::mutex> hotkeyLock(g_hotkeyMainKeysMutex);
        RebuildHotkeyMainKeys_Internal();
        g_configIsDirty = true;

        if (EqualsIgnoreCase(g_config.defaultMode, modeId)) {
            g_config.defaultMode = "Fullscreen";
        }

        if (EqualsIgnoreCase(g_currentModeId, modeId)) {
            std::string fallbackMode = (EqualsIgnoreCase(g_config.defaultMode, modeId) || g_config.defaultMode.empty())
                                           ? "Fullscreen" : g_config.defaultMode;
            std::lock_guard<std::mutex> pendingLock(g_pendingModeSwitchMutex);
            g_pendingModeSwitch.pending = true;
            g_pendingModeSwitch.modeId = fallbackMode;
            g_pendingModeSwitch.source = "Basic mode disabled";
            g_pendingModeSwitch.forceInstant = true;
        }
    };

    auto ModeExists = [&](const std::string& modeId) -> bool {
        for (const auto& mode : g_config.modes) {
            if (EqualsIgnoreCase(mode.id, modeId)) return true;
        }
        return false;
    };

    ImGui::SeparatorText("Modes");

    auto HasHotkeyBound = [&](const std::string& modeId) -> bool {
        for (const auto& hotkey : g_config.hotkeys) {
            if (EqualsIgnoreCase(hotkey.mainMode, "Fullscreen") && EqualsIgnoreCase(hotkey.secondaryMode, modeId)) {
                return !hotkey.keys.empty();
            }
        }
        return false;
    };

    auto RenderModeHotkeyBinding = [&](const std::string& targetModeId, const char* label) {
        int hotkeyIdx = -1;
        for (size_t i = 0; i < g_config.hotkeys.size(); ++i) {
            if (EqualsIgnoreCase(g_config.hotkeys[i].mainMode, "Fullscreen") &&
                EqualsIgnoreCase(g_config.hotkeys[i].secondaryMode, targetModeId)) {
                hotkeyIdx = static_cast<int>(i);
                break;
            }
        }

        if (hotkeyIdx == -1) return;

        std::string keyStr = GetKeyComboString(g_config.hotkeys[hotkeyIdx].keys);
        bool isBinding = (s_mainHotkeyToBind == hotkeyIdx);
        const char* buttonLabel = isBinding ? "[Press Keys...]" : (keyStr.empty() ? "[Click to Bind]" : keyStr.c_str());

        ImGui::PushID(label);
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(40, 60, 100, 180));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(60, 80, 120, 200));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(80, 100, 140, 220));
        float columnWidth = ImGui::GetContentRegionAvail().x;
        if (ImGui::Button(buttonLabel, ImVec2(columnWidth, 0))) {
            s_mainHotkeyToBind = hotkeyIdx;
            s_altHotkeyToBind = { -1, -1 };
            s_exclusionToBind = { -1, -1 };
            MarkHotkeyBindingActive();
        }
        ImGui::PopStyleColor(3);
        ImGui::PopID();
    };

    auto GetModeConfig = [&](const std::string& modeId) -> ModeConfig* {
        for (auto& mode : g_config.modes) {
            if (EqualsIgnoreCase(mode.id, modeId)) return &mode;
        }
        return nullptr;
    };

    enum class EyeZoomInlineKind {
        None,
        LabelsOnly,
        ControlsOnly,
    };

    auto RenderModeTableRow = [&](const std::string& modeId, const char* label, const char* hotkeyLabel, int defaultWidth,
                                  int defaultHeight, int maxWidth, int maxHeight,
                                  EyeZoomInlineKind eyezoomInline = EyeZoomInlineKind::None, bool readOnlyDimensions = false) {
        ModeConfig* modeConfig = GetModeConfig(modeId);

        EnsureHotkeyForMode(modeId);

        ImGui::TableNextRow();

        ImGui::TableNextColumn();
        ImGui::Text("%s", label);

        if (readOnlyDimensions) { ImGui::BeginDisabled(); }

        ImGui::TableNextColumn();
        if (modeConfig) {
            ImGui::PushID((std::string(label) + "_width").c_str());
            if (Spinner("##w", &modeConfig->width, 10, 1, maxWidth, 64, 3)) {
                if (!modeConfig->widthExpr.empty()) { modeConfig->widthExpr.clear(); }
                modeConfig->relativeWidth = -1.0f;
                g_configIsDirty = true;
            }
            ImGui::PopID();
        }

        ImGui::TableNextColumn();
        if (modeConfig) {
            ImGui::PushID((std::string(label) + "_height").c_str());
            if (Spinner("##h", &modeConfig->height, 10, 1, maxHeight, 64, 3)) {
                if (!modeConfig->heightExpr.empty()) { modeConfig->heightExpr.clear(); }
                modeConfig->relativeHeight = -1.0f;
                g_configIsDirty = true;
            }
            ImGui::PopID();
        }

        if (readOnlyDimensions) { ImGui::EndDisabled(); }

        ImGui::TableNextColumn();
        RenderModeHotkeyBinding(modeId, hotkeyLabel);

        ImGui::TableNextColumn();
        if (eyezoomInline != EyeZoomInlineKind::None) {
            ImGui::PushID((std::string("eyezoom_inline_settings_") + modeId).c_str());

            if (ImGui::BeginTable("##eyezoom_inline_tbl", 2, ImGuiTableFlags_SizingStretchSame)) {
                ImGui::TableNextRow();

                if (eyezoomInline == EyeZoomInlineKind::LabelsOnly) {
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted("Clone Width");
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted("Overlay Pixels");
                    ImGui::SameLine();
                    HelpMarker("Clone Width controls how wide the EyeZoom clone samples.\n"
                               "Overlay Pixels controls how much of the numbered overlay is drawn on each side of center.");
                } else if (eyezoomInline == EyeZoomInlineKind::ControlsOnly) {
                    ImGui::TableSetColumnIndex(0);
                    int maxCloneWidth = (modeConfig ? modeConfig->width : maxWidth);
                    if (maxCloneWidth < 2) maxCloneWidth = 2;
                    if (Spinner("##EyeZoomCloneWidth", &g_config.eyezoom.cloneWidth, 2, 2, maxCloneWidth, 64, 3)) {
                        if (g_config.eyezoom.cloneWidth % 2 != 0) { g_config.eyezoom.cloneWidth = (g_config.eyezoom.cloneWidth / 2) * 2; }
                        int maxOverlay = g_config.eyezoom.cloneWidth / 2;
                        if (g_config.eyezoom.overlayWidth > maxOverlay) g_config.eyezoom.overlayWidth = maxOverlay;
                        g_configIsDirty = true;
                    }

                    ImGui::TableSetColumnIndex(1);
                    {
                        int maxOverlay = g_config.eyezoom.cloneWidth / 2;
                        if (Spinner("##EyeZoomOverlayWidth", &g_config.eyezoom.overlayWidth, 1, 0, maxOverlay, 64, 3)) g_configIsDirty = true;
                    }
                }

                ImGui::EndTable();
            }

            ImGui::PopID();
        }
    };

    if (ImGui::BeginTable("ModeTable", 5, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Mode", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Width", ImGuiTableColumnFlags_WidthFixed, 120);
        ImGui::TableSetupColumn("Height", ImGuiTableColumnFlags_WidthFixed, 120);
        ImGui::TableSetupColumn("Hotkey", ImGuiTableColumnFlags_WidthFixed, 150);
        ImGui::TableSetupColumn("EyeZoom Settings", ImGuiTableColumnFlags_WidthFixed, 240);

        ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
        const char* headers[] = { "Mode", "Width", "Height", "Hotkey", "EyeZoom Settings" };
        for (int i = 0; i < 5; i++) {
            ImGui::TableSetColumnIndex(i);
            float columnWidth = ImGui::GetColumnWidth();
            float textWidth = ImGui::CalcTextSize(headers[i]).x;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (columnWidth - textWidth) * 0.5f);
            ImGui::TableHeader(headers[i]);
        }

        int monitorWidth = GetCachedScreenWidth();
        int monitorHeight = GetCachedScreenHeight();

        RenderModeTableRow("Fullscreen", "Fullscreen", "fullscreen_hotkey", monitorWidth, monitorHeight, monitorWidth, monitorHeight,
                           EyeZoomInlineKind::None, true);

        RenderModeTableRow("Thin", "Thin", "thin_hotkey", 400, monitorHeight, monitorWidth, monitorHeight, EyeZoomInlineKind::None);

        RenderModeTableRow("Wide", "Wide", "wide_hotkey", monitorWidth, 400, monitorWidth, monitorHeight, EyeZoomInlineKind::LabelsOnly);

        RenderModeTableRow("EyeZoom", "EyeZoom", "eyezoom_hotkey", 384, 16384, monitorWidth, 16384, EyeZoomInlineKind::ControlsOnly);

        ImGui::EndTable();
    }

    ImGui::SeparatorText("Sensitivity");

    ImGui::Text("Global:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200);
    if (ImGui::SliderFloat("##globalSensBasic", &g_config.mouseSensitivity, 0.001f, 10.0f, "%.3fx")) { g_configIsDirty = true; }
    ImGui::SameLine();
    HelpMarker("Global mouse sensitivity multiplier (1.0 = normal).\nAffects all modes unless overridden.");

    {
        ModeConfig* eyezoomMode = GetModeConfig("EyeZoom");
        if (eyezoomMode) {
            ImGui::Text("EyeZoom:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(200);
            if (ImGui::SliderFloat("##eyezoomSensBasic", &eyezoomMode->modeSensitivity, 0.001f, 10.0f, "%.3fx")) {
                if (eyezoomMode->modeSensitivity < 0.001f) eyezoomMode->modeSensitivity = 0.001f;
                eyezoomMode->sensitivityOverrideEnabled = true;
                g_configIsDirty = true;
            }
            ImGui::SameLine();
            HelpMarker("EyeZoom mode sensitivity (1.0 = normal).\nOverrides global sensitivity when in EyeZoom.");
        }
    }

    ImGui::Separator();
    ImGui::SeparatorText("Overlays");

    {
        auto FindNinjabrainBotImage = [&]() -> ImageConfig* {
            for (auto& img : g_config.images) {
                if (EqualsIgnoreCase(img.name, "Ninjabrain Bot")) { return &img; }
            }
            return nullptr;
        };

        auto CreateNinjabrainBotImage = [&]() {
            WCHAR tempPath[MAX_PATH];
            if (GetTempPathW(MAX_PATH, tempPath) > 0) {
                std::wstring nbImagePath = std::wstring(tempPath) + L"nb-overlay.png";
                ImageConfig ninjabrainBot;
                ninjabrainBot.name = "Ninjabrain Bot";
                ninjabrainBot.path = WideToUtf8(nbImagePath);
                ninjabrainBot.x = 0;
                ninjabrainBot.y = 0;
                ninjabrainBot.scale = 1.2f;
                ninjabrainBot.relativeTo = "topLeft";
                ninjabrainBot.opacity = 1.0f;
                ninjabrainBot.colorKey = { 55 / 255.0f, 60 / 255.0f, 66 / 255.0f };
                ninjabrainBot.enableColorKey = true;
                ninjabrainBot.colorKeySensitivity = 0.05f;
                ninjabrainBot.background = { true, { 0.0f, 0.0f, 0.0f }, 0.5f };
                g_config.images.push_back(ninjabrainBot);
                g_allImagesLoaded = false;
                g_pendingImageLoad = true;
            }
        };

        auto ModeHasNinjabrain = [&](const std::string& modeId) -> bool {
            ModeConfig* mode = GetModeConfig(modeId);
            if (!mode) return false;
            for (const auto& imgId : mode->imageIds) {
                if (EqualsIgnoreCase(imgId, "Ninjabrain Bot")) return true;
            }
            return false;
        };

        auto AddNinjabrainToMode = [&](const std::string& modeId) {
            ModeConfig* mode = GetModeConfig(modeId);
            if (mode && !ModeHasNinjabrain(modeId)) { mode->imageIds.push_back("Ninjabrain Bot"); }
        };

        auto RemoveNinjabrainFromMode = [&](const std::string& modeId) {
            ModeConfig* mode = GetModeConfig(modeId);
            if (mode) {
                mode->imageIds.erase(std::remove_if(mode->imageIds.begin(), mode->imageIds.end(),
                                                    [](const std::string& id) { return EqualsIgnoreCase(id, "Ninjabrain Bot"); }),
                                     mode->imageIds.end());
            }
        };

        bool ninjabrainEnabled =
            ModeHasNinjabrain("Fullscreen") || ModeHasNinjabrain("EyeZoom") || ModeHasNinjabrain("Thin") || ModeHasNinjabrain("Wide");

        if (ImGui::Checkbox("Ninjabrainbot Overlay", &ninjabrainEnabled)) {
            if (ninjabrainEnabled) {
                if (!FindNinjabrainBotImage()) { CreateNinjabrainBotImage(); }
                AddNinjabrainToMode("Fullscreen");
                AddNinjabrainToMode("EyeZoom");
                AddNinjabrainToMode("Thin");
                AddNinjabrainToMode("Wide");
            } else {
                RemoveNinjabrainFromMode("Fullscreen");
                RemoveNinjabrainFromMode("EyeZoom");
                RemoveNinjabrainFromMode("Thin");
                RemoveNinjabrainFromMode("Wide");
            }
            g_configIsDirty = true;
        }
    }

    ImGui::SeparatorText("Mirrors");
    ImGui::TextDisabled("Assign mirrors and mirror groups to modes");

    auto RenderMirrorAssignments = [&](const std::string& modeId, const char* label) {
        ModeConfig* modeConfig = GetModeConfig(modeId);
        if (!modeConfig) return;

        ImGui::PushID(label);
        if (ImGui::TreeNode(label)) {
            int item_idx_to_remove = -1;
            bool remove_is_group = false;

            for (size_t k = 0; k < modeConfig->mirrorIds.size(); ++k) {
                ImGui::PushID(static_cast<int>(k));
                if (ImGui::Button("X", ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()))) {
                    item_idx_to_remove = static_cast<int>(k);
                    remove_is_group = false;
                }
                ImGui::SameLine();
                ImGui::TextUnformatted(modeConfig->mirrorIds[k].c_str());
                ImGui::PopID();
            }

            for (size_t k = 0; k < modeConfig->mirrorGroupIds.size(); ++k) {
                ImGui::PushID(static_cast<int>(k) + 10000);
                if (ImGui::Button("X", ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()))) {
                    item_idx_to_remove = static_cast<int>(k);
                    remove_is_group = true;
                }
                ImGui::SameLine();
                ImGui::Text("[Group] %s", modeConfig->mirrorGroupIds[k].c_str());
                ImGui::PopID();
            }

            if (item_idx_to_remove != -1) {
                if (remove_is_group) {
                    modeConfig->mirrorGroupIds.erase(modeConfig->mirrorGroupIds.begin() + item_idx_to_remove);
                } else {
                    modeConfig->mirrorIds.erase(modeConfig->mirrorIds.begin() + item_idx_to_remove);
                }
                g_configIsDirty = true;
            }

            if (ImGui::BeginCombo("##AddMirrorOrGroup", "[Add Mirror/Group]")) {
                for (const auto& mirrorConf : g_config.mirrors) {
                    if (std::find(modeConfig->mirrorIds.begin(), modeConfig->mirrorIds.end(), mirrorConf.name) ==
                        modeConfig->mirrorIds.end()) {
                        if (ImGui::Selectable(mirrorConf.name.c_str())) {
                            modeConfig->mirrorIds.push_back(mirrorConf.name);
                            g_configIsDirty = true;
                        }
                    }
                }
                if (!g_config.mirrors.empty() && !g_config.mirrorGroups.empty()) { ImGui::Separator(); }
                for (const auto& groupConf : g_config.mirrorGroups) {
                    if (std::find(modeConfig->mirrorGroupIds.begin(), modeConfig->mirrorGroupIds.end(), groupConf.name) ==
                        modeConfig->mirrorGroupIds.end()) {
                        std::string displayName = "[Group] " + groupConf.name;
                        if (ImGui::Selectable(displayName.c_str())) {
                            modeConfig->mirrorGroupIds.push_back(groupConf.name);
                            g_configIsDirty = true;
                        }
                    }
                }
                ImGui::EndCombo();
            }

            ImGui::TreePop();
        }
        ImGui::PopID();
    };

    RenderMirrorAssignments("Fullscreen", "Fullscreen");
    RenderMirrorAssignments("Thin", "Thin");
    RenderMirrorAssignments("Wide", "Wide");
    RenderMirrorAssignments("EyeZoom", "EyeZoom");

    ImGui::EndTabItem();
}


