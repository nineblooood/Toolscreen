if (ImGui::BeginTabItem(trc("tabs.modes"))) {
    g_currentlyEditingMirror = "";
    int mode_to_remove = -1;

    g_imageDragMode.store(false);
    g_windowOverlayDragMode.store(false);

    bool resolutionSupported = IsResolutionChangeSupported(g_gameVersion);
    if (!resolutionSupported) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.7f, 0.0f, 1.0f));
        ImGui::TextWrapped(trc("modes.warning.resolution_change", g_gameVersion.valid ? g_gameVersion.major : 0, g_gameVersion.valid ? g_gameVersion.minor : 0, g_gameVersion.valid ? g_gameVersion.patch : 0));
        ImGui::TextWrapped(trc("modes.features_remain_functional"));
        ImGui::PopStyleColor();
        ImGui::Separator();
    }

    if (g_wmMouseMoveCount.load() > 50) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.5f, 1.0f));
        ImGui::TextWrapped(trc("modes.warning.raw_input"));
        ImGui::PopStyleColor();
        ImGui::Separator();
    }

    SliderCtrlClickTip();
    ImGui::Text(trc("modes.status.current_default_mode", g_config.defaultMode));

    ImGui::SeparatorText(trc("modes.status.default_modes"));

    for (size_t i = 0; i < g_config.modes.size(); ++i) {
        auto& mode = g_config.modes[i];
        if (EqualsIgnoreCase(mode.id, "Fullscreen")) {
            ImGui::PushID((int)i);

            bool node_open = ImGui::TreeNodeEx("##mode_node", ImGuiTreeNodeFlags_SpanAvailWidth, "%s", mode.id.c_str());
            ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ImGui::GetFrameHeight());

            ImGui::Dummy(ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()));

            if (node_open) {

                if (!resolutionSupported) { ImGui::BeginDisabled(); }

                int modeScreenW = GetCachedWindowWidth();
                int modeScreenH = GetCachedWindowHeight();
                if (modeScreenW < 1) modeScreenW = 1;
                if (modeScreenH < 1) modeScreenH = 1;

                bool useManualPixelSize = !mode.useRelativeSize;
                if (ImGui::Checkbox((tr("modes.label.manual_pixel_size") + "##Fullscreen").c_str(), &useManualPixelSize)) {
                    mode.useRelativeSize = !useManualPixelSize;
                    if (mode.useRelativeSize) {
                        mode.widthExpr.clear();
                        mode.heightExpr.clear();
                        mode.relativeWidth = (std::max)(0.01f, (std::min)(1.0f, static_cast<float>(mode.width) / static_cast<float>(modeScreenW)));
                        mode.relativeHeight =
                            (std::max)(0.01f, (std::min)(1.0f, static_cast<float>(mode.height) / static_cast<float>(modeScreenH)));
                    } else {
                        mode.relativeWidth = -1.0f;
                        mode.relativeHeight = -1.0f;
                    }
                    g_configIsDirty = true;
                }

                ImGui::Columns(2, "dims", false);
                ImGui::Text(trc("label.width"));
                ImGui::NextColumn();
                if (mode.useRelativeSize) {
                    float widthPct = ((mode.relativeWidth >= 0.0f && mode.relativeWidth <= 1.0f)
                                          ? mode.relativeWidth
                                          : (static_cast<float>(mode.width) / static_cast<float>(modeScreenW))) *
                                     100.0f;
                    widthPct = (std::max)(1.0f, (std::min)(100.0f, widthPct));
                    if (ImGui::SliderFloat("##WidthPercent", &widthPct, 1.0f, 100.0f, "%.1f%%")) {
                        mode.useRelativeSize = true;
                        mode.widthExpr.clear();
                        mode.relativeWidth = widthPct / 100.0f;
                        mode.width = (std::max)(1, static_cast<int>(mode.relativeWidth * static_cast<float>(modeScreenW)));
                        g_configIsDirty = true;
                        if (g_currentModeId == mode.id) {
                            HWND hwnd = g_minecraftHwnd.load();
                            if (hwnd) { PostMessage(hwnd, WM_SIZE, SIZE_RESTORED, MAKELPARAM(mode.width, mode.height)); }
                        }
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("(%d px)", mode.width);
                } else {
                    int tempWidth = mode.width;
                    if (Spinner("##Width", &tempWidth, 1, 1)) {
                        std::lock_guard<std::mutex> lock(g_pendingDimensionChangeMutex);
                        g_pendingDimensionChange.pending = true;
                        g_pendingDimensionChange.modeId = mode.id;
                        g_pendingDimensionChange.newWidth = tempWidth;
                        g_pendingDimensionChange.newHeight = 0;
                        g_pendingDimensionChange.sendWmSize = (g_currentModeId == mode.id);
                    }
                }
                ImGui::NextColumn();
                ImGui::Text(trc("label.height"));
                ImGui::NextColumn();
                if (mode.useRelativeSize) {
                    float heightPct = ((mode.relativeHeight >= 0.0f && mode.relativeHeight <= 1.0f)
                                           ? mode.relativeHeight
                                           : (static_cast<float>(mode.height) / static_cast<float>(modeScreenH))) *
                                      100.0f;
                    heightPct = (std::max)(1.0f, (std::min)(100.0f, heightPct));
                    if (ImGui::SliderFloat("##HeightPercent", &heightPct, 1.0f, 100.0f, "%.1f%%")) {
                        mode.useRelativeSize = true;
                        mode.heightExpr.clear();
                        mode.relativeHeight = heightPct / 100.0f;
                        mode.height = (std::max)(1, static_cast<int>(mode.relativeHeight * static_cast<float>(modeScreenH)));
                        g_configIsDirty = true;
                        if (g_currentModeId == mode.id) {
                            HWND hwnd = g_minecraftHwnd.load();
                            if (hwnd) { PostMessage(hwnd, WM_SIZE, SIZE_RESTORED, MAKELPARAM(mode.width, mode.height)); }
                        }
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("(%d px)", mode.height);
                } else {
                    int tempHeight = mode.height;
                    if (Spinner("##Height", &tempHeight, 1, 1)) {
                        std::lock_guard<std::mutex> lock(g_pendingDimensionChangeMutex);
                        g_pendingDimensionChange.pending = true;
                        g_pendingDimensionChange.modeId = mode.id;
                        g_pendingDimensionChange.newWidth = 0;
                        g_pendingDimensionChange.newHeight = tempHeight;
                        g_pendingDimensionChange.sendWmSize = (g_currentModeId == mode.id);
                    }
                }
                ImGui::Columns(1);

                if (ImGui::Button(trc("modes.switch_to_this_mode"))) {
                    // Defer mode switch to avoid deadlock (g_configMutex is held during GUI rendering)
                    std::lock_guard<std::mutex> pendingLock(g_pendingModeSwitchMutex);
                    g_pendingModeSwitch.pending = true;
                    g_pendingModeSwitch.modeId = mode.id;
                    g_pendingModeSwitch.source = "GUI mode list";
                    Log("[GUI] Deferred mode switch to: " + mode.id);
                }
                ImGui::SameLine();
                if (ImGui::Button((tr("modes.set_as_default") + "##Fullscreen").c_str())) {
                    g_config.defaultMode = mode.id;
                    g_configIsDirty = true;
                }
                mode.stretch.enabled = true;
                mode.stretch.x = 0;
                mode.stretch.y = 0;
                mode.stretch.width = GetCachedWindowWidth();
                mode.stretch.height = GetCachedWindowHeight();

                ImGui::Separator();
                if (ImGui::TreeNode(trc("modes.transition_settings"))) {
                    RenderTransitionSettingsHorizontalNoBackground(mode, "Fullscreen");
                    ImGui::TreePop();
                }

                if (ImGui::TreeNode(trc("modes.border_settings"))) {
                    if (ImGui::Checkbox(trc("modes.enable_border"), &mode.border.enabled)) { g_configIsDirty = true; }
                    ImGui::SameLine();
                    HelpMarker(trc("modes.tooltip.enable_border"));

                    if (mode.border.enabled) {
                        ImGui::Text(trc("images.border_color"));
                        ImVec4 borderCol = ImVec4(mode.border.color.r, mode.border.color.g, mode.border.color.b, 1.0f);
                        if (ImGui::ColorEdit3("##BorderColor", (float*)&borderCol, ImGuiColorEditFlags_NoInputs)) {
                            mode.border.color = { borderCol.x, borderCol.y, borderCol.z };
                            g_configIsDirty = true;
                        }

                        ImGui::Text(trc("images.border_width"));
                        ImGui::SetNextItemWidth(100);
                        if (Spinner("##BorderWidth", &mode.border.width, 1, 1, 50)) { g_configIsDirty = true; }
                        ImGui::SameLine();
                        ImGui::TextDisabled(trc("label.px"));

                        ImGui::Text(trc("images.border_radius"));
                        ImGui::SetNextItemWidth(100);
                        if (Spinner("##BorderRadius", &mode.border.radius, 1, 0, 100)) { g_configIsDirty = true; }
                        ImGui::SameLine();
                        ImGui::TextDisabled(trc("label.px"));
                    }
                    ImGui::TreePop();
                }
                ImGui::Separator();

                if (ImGui::TreeNode(trc("modes.mirrors"))) {
                    int mirror_idx_to_remove = -1;
                    for (size_t k = 0; k < mode.mirrorIds.size(); ++k) {
                        ImGui::PushID(static_cast<int>(k));
                        std::string del_mirror_label = "X##del_mirror_from_mode_" + std::to_string(k);
                        if (ImGui::Button(del_mirror_label.c_str())) { mirror_idx_to_remove = (int)k; }
                        ImGui::SameLine();
                        ImGui::TextUnformatted(mode.mirrorIds[k].c_str());
                        ImGui::PopID();
                    }
                    if (mirror_idx_to_remove != -1) {
                        mode.mirrorIds.erase(mode.mirrorIds.begin() + mirror_idx_to_remove);
                        g_configIsDirty = true;
                    }
                    if (ImGui::BeginCombo((tr("modes.add_mirror") + "##add_mirror_to_mode").c_str(), trc("modes.select_mirror"))) {
                        for (const auto& mirrorConf : g_config.mirrors) {
                            if (std::find(mode.mirrorIds.begin(), mode.mirrorIds.end(), mirrorConf.name) == mode.mirrorIds.end()) {
                                if (ImGui::Selectable(mirrorConf.name.c_str())) {
                                    mode.mirrorIds.push_back(mirrorConf.name);
                                    g_configIsDirty = true;
                                }
                            }
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::TreePop();
                }

                if (ImGui::TreeNode(trc("modes.mirror_groups"))) {
                    int group_idx_to_remove = -1;
                    for (size_t k = 0; k < mode.mirrorGroupIds.size(); ++k) {
                        ImGui::PushID(static_cast<int>(k));
                        std::string del_group_label = "X##del_mirror_group_from_mode_" + std::to_string(k);
                        if (ImGui::Button(del_group_label.c_str())) { group_idx_to_remove = (int)k; }
                        ImGui::SameLine();
                        ImGui::TextUnformatted(mode.mirrorGroupIds[k].c_str());
                        ImGui::PopID();
                    }
                    if (group_idx_to_remove != -1) {
                        mode.mirrorGroupIds.erase(mode.mirrorGroupIds.begin() + group_idx_to_remove);
                        g_configIsDirty = true;
                    }
                    if (ImGui::BeginCombo((tr("modes.add_mirror_group") + "##add_mirror_group_to_mode").c_str(), trc("modes.select_mirror_group"))) {
                        for (const auto& groupConf : g_config.mirrorGroups) {
                            if (std::find(mode.mirrorGroupIds.begin(), mode.mirrorGroupIds.end(), groupConf.name) ==
                                mode.mirrorGroupIds.end()) {
                                if (ImGui::Selectable(groupConf.name.c_str())) {
                                    mode.mirrorGroupIds.push_back(groupConf.name);
                                    g_configIsDirty = true;
                                }
                            }
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::TreePop();
                }

                if (ImGui::TreeNode(trc("modes.images"))) {
                    int image_idx_to_remove = -1;
                    for (size_t k = 0; k < mode.imageIds.size(); ++k) {
                        ImGui::PushID(static_cast<int>(k));
                        std::string del_img_label = "X##del_img_from_mode_" + std::to_string(k);
                        if (ImGui::Button(del_img_label.c_str())) { image_idx_to_remove = (int)k; }
                        ImGui::SameLine();
                        ImGui::TextUnformatted(mode.imageIds[k].c_str());
                        ImGui::PopID();
                    }
                    if (image_idx_to_remove != -1) {
                        mode.imageIds.erase(mode.imageIds.begin() + image_idx_to_remove);
                        g_configIsDirty = true;
                    }
                    if (ImGui::BeginCombo((tr("modes.add_image") + "##add_image_to_mode").c_str(), trc("modes.select_image"))) {
                        for (const auto& imgConf : g_config.images) {
                            if (std::find(mode.imageIds.begin(), mode.imageIds.end(), imgConf.name) == mode.imageIds.end()) {
                                if (ImGui::Selectable(imgConf.name.c_str())) {
                                    mode.imageIds.push_back(imgConf.name);
                                    g_configIsDirty = true;
                                }
                            }
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::TreePop();
                }

                if (ImGui::TreeNode(trc("modes.window_overlays"))) {
                    int windowOverlay_idx_to_remove = -1;
                    for (size_t k = 0; k < mode.windowOverlayIds.size(); ++k) {
                        ImGui::PushID(static_cast<int>(k));
                        std::string del_overlay_label = "X##del_overlay_from_mode_" + std::to_string(k);
                        if (ImGui::Button(del_overlay_label.c_str())) { windowOverlay_idx_to_remove = (int)k; }
                        ImGui::SameLine();
                        ImGui::TextUnformatted(mode.windowOverlayIds[k].c_str());
                        ImGui::PopID();
                    }
                    if (windowOverlay_idx_to_remove != -1) {
                        mode.windowOverlayIds.erase(mode.windowOverlayIds.begin() + windowOverlay_idx_to_remove);
                        g_configIsDirty = true;
                    }
                    if (ImGui::BeginCombo((tr("modes.add_window_overlay") + "##add_overlay_to_mode").c_str(), trc("modes.select_overlay"))) {
                        for (const auto& overlayConf : g_config.windowOverlays) {
                            if (std::find(mode.windowOverlayIds.begin(), mode.windowOverlayIds.end(), overlayConf.name) ==
                                mode.windowOverlayIds.end()) {
                                if (ImGui::Selectable(overlayConf.name.c_str())) {
                                    mode.windowOverlayIds.push_back(overlayConf.name);
                                    g_configIsDirty = true;
                                }
                            }
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::TreePop();
                }


                if (ImGui::TreeNode((tr("modes.sensitivity_override") + "##Fullscreen").c_str())) {
                    if (ImGui::Checkbox(trc("modes.override_sensitivity"), &mode.sensitivityOverrideEnabled)) { g_configIsDirty = true; }
                    HelpMarker(trc("modes.tooltip.override_sensitivity"));

                    if (mode.sensitivityOverrideEnabled) {
                        if (ImGui::Checkbox(trc("modes.separate_xy_sensitivity"), &mode.separateXYSensitivity)) {
                            g_configIsDirty = true;
                            if (mode.separateXYSensitivity) {
                                mode.modeSensitivityX = mode.modeSensitivity;
                                mode.modeSensitivityY = mode.modeSensitivity;
                            }
                        }
                        ImGui::SameLine();
                        HelpMarker(trc("modes.tooltip.separate_xy_sensitivity"));

                        if (mode.separateXYSensitivity) {
                            RawInputSensitivityNote();
                            ImGui::Text(trc("modes.sensitivity_x"));
                            ImGui::SetNextItemWidth(200);
                            if (ImGui::SliderFloat("##FullscreenSensitivityX", &mode.modeSensitivityX, 0.001f, 10.0f, "%.3fx")) {
                                g_configIsDirty = true;
                            }
                            RawInputSensitivityNote();
                            ImGui::Text(trc("modes.sensitivity_y"));
                            ImGui::SetNextItemWidth(200);
                            if (ImGui::SliderFloat("##FullscreenSensitivityY", &mode.modeSensitivityY, 0.001f, 10.0f, "%.3fx")) {
                                g_configIsDirty = true;
                            }
                        } else {
                            RawInputSensitivityNote();
                            ImGui::Text(trc("modes.sensitivity"));
                            ImGui::SetNextItemWidth(200);
                            if (ImGui::SliderFloat("##FullscreenSensitivity", &mode.modeSensitivity, 0.001f, 10.0f, "%.3fx")) {
                                g_configIsDirty = true;
                            }
                            ImGui::SameLine();
                            HelpMarker(trc("modes.tooltip.sensitivity"));
                        }
                    }
                    ImGui::TreePop();
                }

                if (!resolutionSupported) { ImGui::EndDisabled(); }

                ImGui::TreePop();
            }
            ImGui::PopID();
            break;
        }
    }

    for (size_t i = 0; i < g_config.modes.size(); ++i) {
        auto& mode = g_config.modes[i];
        if (EqualsIgnoreCase(mode.id, "EyeZoom")) {
            ImGui::PushID((int)i + 10000);

            bool node_open = ImGui::TreeNodeEx("##mode_node", ImGuiTreeNodeFlags_SpanAvailWidth, "%s", mode.id.c_str());
            ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ImGui::GetFrameHeight());

            ImGui::Dummy(ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()));

            if (node_open) {
                if (!resolutionSupported) { ImGui::BeginDisabled(); }

                ImGui::Columns(2, "mode_config_cols", false);
                ImGui::SetColumnWidth(0, 150);

                ImGui::Text(trc("modes.game_width"));
                ImGui::NextColumn();
                int tempWidth2 = mode.width;
                if (Spinner("##ModeWidth", &tempWidth2, 1, 1, screenWidth)) {
                    std::lock_guard<std::mutex> lock(g_pendingDimensionChangeMutex);
                    g_pendingDimensionChange.pending = true;
                    g_pendingDimensionChange.modeId = mode.id;
                    g_pendingDimensionChange.newWidth = tempWidth2;
                    g_pendingDimensionChange.newHeight = 0;
                    g_pendingDimensionChange.sendWmSize = (g_currentModeId == mode.id);
                }
                ImGui::NextColumn();
                ImGui::Text(trc("modes.game_height"));
                ImGui::NextColumn();
                int tempHeight2 = mode.height;
                if (Spinner("##ModeHeight", &tempHeight2, 1, 1, 16384)) {
                    std::lock_guard<std::mutex> lock(g_pendingDimensionChangeMutex);
                    g_pendingDimensionChange.pending = true;
                    g_pendingDimensionChange.modeId = mode.id;
                    g_pendingDimensionChange.newWidth = 0;
                    g_pendingDimensionChange.newHeight = tempHeight2;
                    g_pendingDimensionChange.sendWmSize = (g_currentModeId == mode.id);
                }
                ImGui::Columns(1);

                if (ImGui::Button(trc("modes.switch_to_this_mode"))) {
                    // Defer mode switch to avoid deadlock (g_configMutex is held during GUI rendering)
                    std::lock_guard<std::mutex> pendingLock(g_pendingModeSwitchMutex);
                    g_pendingModeSwitch.pending = true;
                    g_pendingModeSwitch.modeId = mode.id;
                    g_pendingModeSwitch.source = "GUI EyeZoom mode";
                    Log("[GUI] Deferred mode switch to: " + mode.id);
                }
                ImGui::SameLine();
                if (ImGui::Button((tr("modes.set_as_default") + "##EyeZoom").c_str())) {
                    g_config.defaultMode = mode.id;
                    g_configIsDirty = true;
                }

                if (!resolutionSupported) { ImGui::EndDisabled(); }

                if (g_currentModeId == mode.id) {
                    ImGui::SameLine();
                    ImGui::TextDisabled(trc("label.current"));
                }

                ImGui::Separator();
                ImGui::Text(trc("modes.eyezoom.settings"));

                ImGui::Text(trc("modes.eyezoom.clone_settings_source"));
                ImGui::Columns(2, "eyezoom_clone_cols", false);
                ImGui::SetColumnWidth(0, 150);
                ImGui::Text(trc("modes.eyezoom.clone_width"));
                ImGui::NextColumn();
                // Clone Width must be even - step by 2, enforce even values
                int maxCloneWidth = mode.width;
                if (Spinner("##EyeZoomCloneWidth", &g_config.eyezoom.cloneWidth, 2, 2, maxCloneWidth)) {
                    if (g_config.eyezoom.cloneWidth % 2 != 0) { g_config.eyezoom.cloneWidth = (g_config.eyezoom.cloneWidth / 2) * 2; }
                    int maxOverlay = g_config.eyezoom.cloneWidth / 2;
                    if (g_config.eyezoom.overlayWidth > maxOverlay) g_config.eyezoom.overlayWidth = maxOverlay;
                    g_configIsDirty = true;
                }
                ImGui::NextColumn();
                ImGui::Text(trc("modes.eyezoom.clone_height"));
                ImGui::NextColumn();
                int maxCloneHeight = mode.height;
                if (Spinner("##EyeZoomCloneHeight", &g_config.eyezoom.cloneHeight, 10, 1, maxCloneHeight)) g_configIsDirty = true;
                ImGui::Columns(1);

                ImGui::Separator();
                ImGui::Text(trc("label.overlay"));

                // -- Default overlay (built-in numbered boxes) --
                {
                    if (ImGui::RadioButton("##ezov_radio_default", g_config.eyezoom.activeOverlayIndex == -1)) {
                        g_config.eyezoom.activeOverlayIndex = -1;
                        g_configIsDirty = true;
                    }
                    ImGui::SameLine();
                    bool defaultNodeOpen = ImGui::TreeNodeEx("##ezoverlay_default_node", ImGuiTreeNodeFlags_SpanAvailWidth, "Default");
                    if (defaultNodeOpen) {
                        int maxOverlay = g_config.eyezoom.cloneWidth / 2;
                        ImGui::Text(trc("modes.eyezoom.overlay_pixels"));
                        ImGui::SameLine();
                        if (Spinner("##EyeZoomOverlayWidth", &g_config.eyezoom.overlayWidth, 1, 0, maxOverlay)) g_configIsDirty = true;
                        ImGui::SameLine();
                        HelpMarker(trc("modes.eyezoom.tooltip.overlay_pixels"));
                        ImGui::TreePop();
                    }
                }

                // -- Custom overlays (user-added images) --
                int ezoverlay_to_remove = -1;
                for (size_t ovi = 0; ovi < g_config.eyezoom.overlays.size(); ++ovi) {
                    auto& ov = g_config.eyezoom.overlays[ovi];
                    ImGui::PushID(static_cast<int>(ovi));

                    std::string deleteLabel = "X##delete_ezoverlay_" + std::to_string(ovi);
                    if (ImGui::Button(deleteLabel.c_str(), ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()))) {
                        std::string popupId = (tr("modes.eyezoom.delete_overlay") + "##ezov_" + std::to_string(ovi));
                    ImGui::OpenPopup(popupId.c_str());
                }

                    {
                        std::string popupId = (tr("modes.eyezoom.delete_overlay") + "##ezov_" + std::to_string(ovi));
                        if (ImGui::BeginPopupModal(popupId.c_str(), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
                            ImGui::Text(trc("modes.eyezoom.delete_overlay_confirm", ov.name));
                            ImGui::Separator();
                            if (ImGui::Button(trc("button.ok"), ImVec2(120, 0))) {
                                ezoverlay_to_remove = (int)ovi;
                                g_configIsDirty = true;
                                ImGui::CloseCurrentPopup();
                            }
                            ImGui::SameLine();
                            if (ImGui::Button(trc("button.cancel"), ImVec2(120, 0))) { ImGui::CloseCurrentPopup(); }
                            ImGui::EndPopup();
                        }
                    }

                    ImGui::SameLine();
                    if (ImGui::RadioButton("##ezov_radio", g_config.eyezoom.activeOverlayIndex == (int)ovi)) {
                        g_config.eyezoom.activeOverlayIndex = (int)ovi;
                        g_configIsDirty = true;
                    }
                    ImGui::SameLine();

                    std::string oldName = ov.name;
                    bool nodeOpen = ImGui::TreeNodeEx("##ezoverlay_node", ImGuiTreeNodeFlags_SpanAvailWidth, "%s", ov.name.c_str());

                    if (nodeOpen) {
                        bool hasDuplicate = HasDuplicateEyeZoomOverlayName(ov.name, ovi);
                        if (hasDuplicate) {
                            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.6f, 0.2f, 0.2f, 1.0f));
                            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.7f, 0.3f, 0.3f, 1.0f));
                            ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.8f, 0.3f, 0.3f, 1.0f));
                        }

                        if (ImGui::InputText("Name##ezov", &ov.name)) {
                            if (!HasDuplicateEyeZoomOverlayName(ov.name, ovi)) {
                                g_configIsDirty = true;
                            } else {
                                ov.name = oldName;
                            }
                        }

                        if (hasDuplicate) { ImGui::PopStyleColor(3); }
                        if (hasDuplicate) {
                            ImGui::SameLine();
                            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), trc("images.name_duplicate"));
                        }

                        if (ImGui::InputText((tr("label.path") + "##ezov").c_str(), &ov.path)) {
                            g_configIsDirty = true;
                        }
                        if (ImGui::IsItemDeactivatedAfterEdit() && !ov.path.empty()) {
                            LoadImageAsync(DecodedImageData::UserImage, "ezoverlay_" + ov.name, ov.path, g_toolscreenPath);
                        }

                        ImGui::SameLine();
                        if (ImGui::Button((tr("button.browse") + "##ezov_" + std::to_string(ovi)).c_str())) {
                            ImagePickerResult result = OpenImagePickerAndValidate(g_minecraftHwnd.load(), g_toolscreenPath, g_toolscreenPath);
                            if (result.completed) {
                                if (result.success) {
                                    ov.path = result.path;
                                    g_configIsDirty = true;
                                    LoadImageAsync(DecodedImageData::UserImage, "ezoverlay_" + ov.name, ov.path, g_toolscreenPath);
                                }
                            }
                        }

                        // File-found badge
                        if (!ov.path.empty()) {
                            std::wstring ovWpath = Utf8ToWide(ov.path);
                            std::wstring ovFinalPath = (PathIsRelativeW(ovWpath.c_str()) && !g_toolscreenPath.empty())
                                ? (g_toolscreenPath + L"\\" + ovWpath) : ovWpath;
                            if (std::filesystem::exists(ovFinalPath)) {
                                ImGui::SameLine();
                                ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), trc("label.file_exists"));
                                if (ImGui::IsItemHovered()) ImGui::SetTooltip(trc("label.file_exists"));
                            } else {
                                ImGui::SameLine();
                                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "(!)");
                                if (ImGui::IsItemHovered()) ImGui::SetTooltip(trc("error.file_not_exists"));
                            }
                        }

                        ImGui::Text(trc("label.display_mode"));
                        ImGui::SameLine();
                        int dispMode = static_cast<int>(ov.displayMode);
                        if (ImGui::RadioButton((tr("label.manual") + "##ezov").c_str(), &dispMode, 0)) { ov.displayMode = EyeZoomOverlayDisplayMode::Manual; g_configIsDirty = true; }
                        ImGui::SameLine();
                        if (ImGui::RadioButton((tr("label.fit") + "##ezov").c_str(), &dispMode, 1)) { ov.displayMode = EyeZoomOverlayDisplayMode::Fit; g_configIsDirty = true; }
                        ImGui::SameLine();
                        if (ImGui::RadioButton((tr("label.stretch") + "##ezov").c_str(), &dispMode, 2)) { ov.displayMode = EyeZoomOverlayDisplayMode::Stretch; g_configIsDirty = true; }

                        if (ov.displayMode == EyeZoomOverlayDisplayMode::Manual) {
                            if (ImGui::SliderInt((tr("label.width") + "##ezov_mw").c_str(), &ov.manualWidth, 1, 4096)) g_configIsDirty = true;
                            if (ImGui::SliderInt((tr("label.height") + "##ezov_mh").c_str(), &ov.manualHeight, 1, 4096)) g_configIsDirty = true;
                        }

                        if (ImGui::SliderFloat((tr("label.opacity") + "##ezov").c_str(), &ov.opacity, 0.0f, 1.0f)) g_configIsDirty = true;

                        ImGui::TreePop();
                    }
                    ImGui::PopID();
                }

                if (ezoverlay_to_remove != -1) {
                    g_config.eyezoom.overlays.erase(g_config.eyezoom.overlays.begin() + ezoverlay_to_remove);
                    if (g_config.eyezoom.activeOverlayIndex == ezoverlay_to_remove) {
                        g_config.eyezoom.activeOverlayIndex = -1; // fall back to Default
                    } else if (g_config.eyezoom.activeOverlayIndex > ezoverlay_to_remove) {
                        g_config.eyezoom.activeOverlayIndex--;
                    }
                    g_configIsDirty = true;
                }

                if (ImGui::Button(trc("modes.eyezoom.add_overlay"))) {
                    EyeZoomOverlayConfig newOv;
                    newOv.name = tr("modes.eyezoom.overlay") + " " + std::to_string(g_config.eyezoom.overlays.size() + 1);
                    g_config.eyezoom.overlays.push_back(newOv);
                    g_configIsDirty = true;
                }

                ImGui::Separator();
                ImGui::Text(trc("label.placement"));
                if (ImGui::Checkbox(trc("modes.eyezoom.use_custom_position"), &g_config.eyezoom.useCustomSizePosition)) { g_configIsDirty = true; }
                ImGui::SameLine();
                HelpMarker(trc("modes.eyezoom.tooltip.use_custom_position"));

                int monitorHeight = GetCachedWindowHeight();
                if (monitorHeight < 1) monitorHeight = 1;
                int monitorWidth = screenWidth;
                if (monitorWidth < 1) monitorWidth = 1;

                if (!g_config.eyezoom.useCustomSizePosition) {
                    int eyezoomModeWidth = mode.width;
                    int eyezoomTargetFinalX = (monitorWidth - eyezoomModeWidth) / 2;
                    if (eyezoomTargetFinalX < 1) eyezoomTargetFinalX = 1;

                    int autoHorizontalMargin = eyezoomTargetFinalX / 10;
                    int autoZoomAreaWidth = eyezoomTargetFinalX - (2 * autoHorizontalMargin);
                    if (autoZoomAreaWidth < 1) autoZoomAreaWidth = 1;

                    int autoVerticalMargin = monitorHeight / 8;
                    int autoZoomAreaHeight = monitorHeight - (2 * autoVerticalMargin);
                    if (autoZoomAreaHeight < 1) autoZoomAreaHeight = 1;

                    int autoPosY = (monitorHeight - autoZoomAreaHeight) / 2;
                    ImGui::TextDisabled(trc("label.auto_layout_format", autoHorizontalMargin, autoPosY, autoZoomAreaWidth,
                                        autoZoomAreaHeight));
                }

                ImGui::BeginDisabled(!g_config.eyezoom.useCustomSizePosition);

                ImGui::Separator();
                ImGui::Text(trc("modes.eyezoom.zoom_area_output"));
                ImGui::Columns(2, "eyezoom_area_cols", false);
                ImGui::SetColumnWidth(0, 150);
                ImGui::Text(trc("modes.eyezoom.zoom_area_width"));
                ImGui::NextColumn();
                constexpr int kEyeZoomMaxCustomDimension = 16384;
                constexpr int kEyeZoomMaxCustomPosition = 16384;
                int maxZoomAreaWidth = (std::max)(kEyeZoomMaxCustomDimension, g_config.eyezoom.zoomAreaWidth);
                int maxZoomAreaHeight = (std::max)(kEyeZoomMaxCustomDimension, g_config.eyezoom.zoomAreaHeight);
                if (Spinner("##EyeZoomAreaWidth", &g_config.eyezoom.zoomAreaWidth, 10, 1, maxZoomAreaWidth)) g_configIsDirty = true;
                ImGui::NextColumn();
                ImGui::Text(trc("modes.eyezoom.zoom_area_height"));
                ImGui::NextColumn();
                if (Spinner("##EyeZoomAreaHeight", &g_config.eyezoom.zoomAreaHeight, 10, 1, maxZoomAreaHeight)) g_configIsDirty = true;
                ImGui::Columns(1);

                ImGui::Separator();
                HelpMarker("Set the EyeZoom output rectangle size, then place it anywhere on screen using X/Y.");

                int maxPosX = (std::max)(kEyeZoomMaxCustomPosition, g_config.eyezoom.positionX);
                int maxPosY = (std::max)(kEyeZoomMaxCustomPosition, g_config.eyezoom.positionY);

                ImGui::Columns(2, "eyezoom_position_cols", false);
                ImGui::SetColumnWidth(0, 150);
                ImGui::Text(trc("label.position_x"));
                ImGui::NextColumn();
                if (Spinner("##EyeZoomPositionX", &g_config.eyezoom.positionX, 10, 0, maxPosX)) g_configIsDirty = true;
                ImGui::NextColumn();
                ImGui::Text(trc("label.position_y"));
                ImGui::NextColumn();
                if (Spinner("##EyeZoomPositionY", &g_config.eyezoom.positionY, 10, 0, maxPosY)) g_configIsDirty = true;
                ImGui::Columns(1);

                ImGui::EndDisabled();

                ImGui::Separator();
                ImGui::Text(trc("modes.eyezoom.color_settings"));
                {
                    ImVec4 col1 = ImVec4(g_config.eyezoom.gridColor1.r, g_config.eyezoom.gridColor1.g, g_config.eyezoom.gridColor1.b,
                                         g_config.eyezoom.gridColor1Opacity);
                    if (ImGui::ColorEdit4(trc("modes.eyezoom.grid_color_1"), (float*)&col1, ImGuiColorEditFlags_AlphaBar)) {
                        g_config.eyezoom.gridColor1 = { col1.x, col1.y, col1.z };
                        g_config.eyezoom.gridColor1Opacity = col1.w;
                        g_configIsDirty = true;
                    }
                }
                {
                    ImVec4 col2 = ImVec4(g_config.eyezoom.gridColor2.r, g_config.eyezoom.gridColor2.g, g_config.eyezoom.gridColor2.b,
                                         g_config.eyezoom.gridColor2Opacity);
                    if (ImGui::ColorEdit4(trc("modes.eyezoom.grid_color_2"), (float*)&col2, ImGuiColorEditFlags_AlphaBar)) {
                        g_config.eyezoom.gridColor2 = { col2.x, col2.y, col2.z };
                        g_config.eyezoom.gridColor2Opacity = col2.w;
                        g_configIsDirty = true;
                    }
                }
                {
                    ImVec4 col3 = ImVec4(g_config.eyezoom.centerLineColor.r, g_config.eyezoom.centerLineColor.g,
                                         g_config.eyezoom.centerLineColor.b, g_config.eyezoom.centerLineColorOpacity);
                    if (ImGui::ColorEdit4(trc("modes.eyezoom.center_line_color"), (float*)&col3, ImGuiColorEditFlags_AlphaBar)) {
                        g_config.eyezoom.centerLineColor = { col3.x, col3.y, col3.z };
                        g_config.eyezoom.centerLineColorOpacity = col3.w;
                        g_configIsDirty = true;
                    }
                }
                {
                    ImVec4 col4 = ImVec4(g_config.eyezoom.textColor.r, g_config.eyezoom.textColor.g, g_config.eyezoom.textColor.b,
                                         g_config.eyezoom.textColorOpacity);
                    if (ImGui::ColorEdit4(trc("modes.eyezoom.text_color"), (float*)&col4, ImGuiColorEditFlags_AlphaBar)) {
                        g_config.eyezoom.textColor = { col4.x, col4.y, col4.z };
                        g_config.eyezoom.textColorOpacity = col4.w;
                        g_configIsDirty = true;
                    }
                }

                ImGui::Separator();
                ImGui::Text(trc("modes.eyezoom.text_settings"));
                if (ImGui::Checkbox(trc("modes.eyezoom.auto_font_size"), &g_config.eyezoom.autoFontSize)) {
                    g_configIsDirty = true;
                }
                ImGui::SameLine();
                HelpMarker(trc("modes.eyezoom.tooltip.auto_font_size"));

                if (!g_config.eyezoom.autoFontSize) {
                    ImGui::SetNextItemWidth(250);
                    if (ImGui::SliderInt(trc("modes.eyezoom.text_font_size"), &g_config.eyezoom.textFontSize, 1, 96)) {
                        g_configIsDirty = true;
                        SetOverlayTextFontSize(g_config.eyezoom.textFontSize);
                    }
                } else {
                    ImGui::TextDisabled(trc("modes.eyezoom.tooltip.font_size_clamped"));
                    if (g_config.eyezoom.linkRectToFont) {
                        ImGui::TextDisabled(trc("modes.eyezoom.tooltip.link_rect_to_font"));
                    }
                }

                ImGui::Text(trc("modes.eyezoom.text_font"));
                ImGui::SetNextItemWidth(300);
                if (ImGui::InputText("##EyeZoomTextFont", &g_config.eyezoom.textFontPath)) {
                    g_configIsDirty = true;
                    g_eyeZoomFontNeedsReload.store(true);
                }
                ImGui::SameLine();
                if (ImGui::Button((tr("button.browse") + "##EyeZoomFont").c_str())) {
                    OPENFILENAMEA ofn = {};
                    char szFile[MAX_PATH] = {};

                    if (!g_config.eyezoom.textFontPath.empty()) { strncpy_s(szFile, g_config.eyezoom.textFontPath.c_str(), MAX_PATH - 1); }

                    ofn.lStructSize = sizeof(ofn);
                    ofn.hwndOwner = g_minecraftHwnd.load();
                    ofn.lpstrFile = szFile;
                    ofn.nMaxFile = sizeof(szFile);
                    ofn.lpstrFilter = "Font Files (*.ttf;*.otf)\0*.ttf;*.otf\0All Files (*.*)\0*.*\0";
                    ofn.nFilterIndex = 1;
                    ofn.lpstrTitle = "Select Font for EyeZoom Text";
                    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
                    ofn.lpstrInitialDir = "C:\\Windows\\Fonts";

                    if (GetOpenFileNameA(&ofn)) {
                        g_config.eyezoom.textFontPath = szFile;
                        g_configIsDirty = true;
                        g_eyeZoomFontNeedsReload.store(true);
                    }
                }
                ImGui::SameLine();
                HelpMarker(trc("modes.eyezoom.tooltip.custom_font"));

                if (ImGui::Checkbox(trc("modes.eyezoom.link_rect_to_font"), &g_config.eyezoom.linkRectToFont)) {
                    g_configIsDirty = true;
                    if (g_config.eyezoom.linkRectToFont) {
                        g_config.eyezoom.rectHeight = static_cast<int>(g_config.eyezoom.textFontSize * 1.2f);
                    }
                }

                if (!g_config.eyezoom.linkRectToFont) {
                    ImGui::SetNextItemWidth(250);
                    if (ImGui::SliderInt("Override Rectangle Height (px)", &g_config.eyezoom.rectHeight, 8, 120)) {
                        g_configIsDirty = true;
                    }
                }

                if (ImGui::TreeNode(trc("modes.background"))) {
                    if (ImGui::RadioButton(trc("modes.color"), mode.background.selectedMode == "color")) {
                        if (mode.background.selectedMode != "color") {
                            mode.background.selectedMode = "color";
                            g_configIsDirty = true;
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::RadioButton(trc("modes.gradient"), mode.background.selectedMode == "gradient")) {
                        if (mode.background.selectedMode != "gradient") {
                            mode.background.selectedMode = "gradient";
                            if (mode.background.gradientStops.size() < 2) {
                                mode.background.gradientStops.clear();
                                mode.background.gradientStops.push_back({ { 0.0f, 0.0f, 0.0f }, 0.0f });
                                mode.background.gradientStops.push_back({ { 1.0f, 1.0f, 1.0f }, 1.0f });
                            }
                            g_configIsDirty = true;
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::RadioButton(trc("label.image"), mode.background.selectedMode == "image")) {
                        if (mode.background.selectedMode != "image") {
                            mode.background.selectedMode = "image";
                            g_configIsDirty = true;
                            if (!mode.background.image.empty()) {
                                g_allImagesLoaded = false;
                                g_pendingImageLoad = true;
                                LoadImageAsync(DecodedImageData::Type::Background, mode.id, mode.background.image, g_toolscreenPath);
                            }
                        }
                    }

                    if (mode.background.selectedMode == "color") {
                        if (ImGui::ColorEdit3("##bgColor", &mode.background.color.r)) { g_configIsDirty = true; }
                    } else if (mode.background.selectedMode == "gradient") {
                        ImGui::SetNextItemWidth(200);
                        if (ImGui::SliderFloat("Angle##bgGradAngle", &mode.background.gradientAngle, 0.0f, 360.0f, "%.0f deg")) {
                            g_configIsDirty = true;
                        }

                        ImGui::Text(trc("modes.color_stops"));
                        int stopToRemove = -1;
                        for (size_t i = 0; i < mode.background.gradientStops.size(); i++) {
                            ImGui::PushID(static_cast<int>(i));
                            auto& stop = mode.background.gradientStops[i];

                            if (ImGui::ColorEdit3("##StopColor", &stop.color.r, ImGuiColorEditFlags_NoInputs)) { g_configIsDirty = true; }
                            ImGui::SameLine();

                            float pos = stop.position * 100.0f;
                            ImGui::SetNextItemWidth(100);
                            if (ImGui::SliderFloat("##StopPos", &pos, 0.0f, 100.0f, "%.0f%%")) {
                                stop.position = pos / 100.0f;
                                g_configIsDirty = true;
                            }

                            if (mode.background.gradientStops.size() > 2) {
                                ImGui::SameLine();
                                if (ImGui::Button("X##RemoveStop")) { stopToRemove = static_cast<int>(i); }
                            }

                            ImGui::PopID();
                        }
                        if (stopToRemove >= 0) {
                            mode.background.gradientStops.erase(mode.background.gradientStops.begin() + stopToRemove);
                            g_configIsDirty = true;
                        }

                        if (mode.background.gradientStops.size() < 8) {
                            if (ImGui::Button((tr("modes.gradient_add_color_stop") + "##bgGrad").c_str())) {
                                GradientColorStop newStop;
                                newStop.position = 0.5f;
                                newStop.color = { 0.5f, 0.5f, 0.5f };
                                mode.background.gradientStops.push_back(newStop);
                                std::sort(mode.background.gradientStops.begin(), mode.background.gradientStops.end(),
                                          [](const GradientColorStop& a, const GradientColorStop& b) { return a.position < b.position; });
                                g_configIsDirty = true;
                            }
                        }

                        ImGui::Separator();
                        ImGui::Text(trc("modes.gradient_animation"));
                        const char* animTypeNames[] = { trc("modes.gradient_animation_none"), trc("modes.gradient_animation_rotate"), trc("modes.gradient_animation_slide"), trc("modes.gradient_animation_wave"), trc("modes.gradient_animation_spiral"), trc("modes.gradient_animation_fade") };
                        int currentAnimType = static_cast<int>(mode.background.gradientAnimation);
                        ImGui::SetNextItemWidth(120);
                        if (ImGui::Combo("Type##GradAnim", &currentAnimType, animTypeNames, IM_ARRAYSIZE(animTypeNames))) {
                            mode.background.gradientAnimation = static_cast<GradientAnimationType>(currentAnimType);
                            g_configIsDirty = true;
                        }
                        if (mode.background.gradientAnimation != GradientAnimationType::None) {
                            ImGui::SetNextItemWidth(150);
                            if (ImGui::SliderFloat("Speed##GradAnimSpeed", &mode.background.gradientAnimationSpeed, 0.1f, 5.0f, "%.1fx")) {
                                g_configIsDirty = true;
                            }
                            /*
                            if (ImGui::Checkbox("Color Fade##GradColorFade", &mode.background.gradientColorFade)) {
                                g_configIsDirty = true;
                            }
                            if (ImGui::IsItemHovered()) {
                                ImGui::SetTooltip("Smoothly cycle colors through gradient stops");
                            }*/
                        }
                    } else if (mode.background.selectedMode == "image") {
                        if (ImGui::InputText("Path", &mode.background.image)) {
                            ClearImageError("eyezoom_bg");
                            g_configIsDirty = true;
                            g_allImagesLoaded = false;
                            g_pendingImageLoad = true;
                        }
                        ImGui::SameLine();
                        if (ImGui::Button((tr("button.browse") + "##eyezoom_bg").c_str())) {
                            ImagePickerResult result =
                                OpenImagePickerAndValidate(g_minecraftHwnd.load(), g_toolscreenPath, g_toolscreenPath);

                            if (result.completed) {
                                if (result.success) {
                                    mode.background.image = result.path;
                                    ClearImageError("eyezoom_bg");
                                    g_allImagesLoaded = false;
                                    g_pendingImageLoad = true;
                                    g_configIsDirty = true;
                                } else if (!result.error.empty()) {
                                    SetImageError("eyezoom_bg", result.error);
                                }
                            }
                        }

                        std::string bgError = GetImageError("eyezoom_bg");
                        if (!bgError.empty()) { ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", bgError.c_str()); }
                    }
                    ImGui::TreePop();
                }

                if (ImGui::TreeNode((tr("modes.border_settings") + "##EyeZoom").c_str())) {
                    if (ImGui::Checkbox((tr("modes.enable_border") + "##EyeZoom").c_str(), &mode.border.enabled)) { g_configIsDirty = true; }
                    ImGui::SameLine();
                    HelpMarker(trc("modes.tooltip.enable_border"));

                    if (mode.border.enabled) {
                        ImGui::Text(trc("images.border_color"));
                        ImVec4 borderCol = ImVec4(mode.border.color.r, mode.border.color.g, mode.border.color.b, 1.0f);
                        if (ImGui::ColorEdit3("##BorderColorEyeZoom", (float*)&borderCol, ImGuiColorEditFlags_NoInputs)) {
                            mode.border.color = { borderCol.x, borderCol.y, borderCol.z };
                            g_configIsDirty = true;
                        }

                        ImGui::Text(trc("images.border_width"));
                        ImGui::SetNextItemWidth(100);
                        if (Spinner("##BorderWidthEyeZoom", &mode.border.width, 1, 1, 50)) { g_configIsDirty = true; }
                        ImGui::SameLine();
                        ImGui::TextDisabled(trc("label.px"));

                        ImGui::Text(trc("images.border_radius"));
                        ImGui::SetNextItemWidth(100);
                        if (Spinner("##BorderRadiusEyeZoom", &mode.border.radius, 1, 0, 100)) { g_configIsDirty = true; }
                        ImGui::SameLine();
                        ImGui::TextDisabled(trc("label.px"));
                    }
                    ImGui::TreePop();
                }

                if (ImGui::TreeNode(trc("modes.eyezoom.mirrors"))) {
                    int mirror_idx_to_remove = -1;
                    for (size_t k = 0; k < mode.mirrorIds.size(); ++k) {
                        ImGui::PushID(static_cast<int>(k));
                        std::string del_mirror_label = "X##del_mirror_from_mode_" + std::to_string(k);
                        if (ImGui::Button(del_mirror_label.c_str())) { mirror_idx_to_remove = (int)k; }
                        ImGui::SameLine();
                        ImGui::TextUnformatted(mode.mirrorIds[k].c_str());
                        ImGui::PopID();
                    }
                    if (mirror_idx_to_remove != -1) {
                        mode.mirrorIds.erase(mode.mirrorIds.begin() + mirror_idx_to_remove);
                        g_configIsDirty = true;
                    }
                    if (ImGui::BeginCombo((tr("modes.add_mirror") + "##add_mirror_to_eyezoom").c_str(), trc("modes.select_mirror"))) {
                        for (const auto& mirrorConf : g_config.mirrors) {
                            if (std::find(mode.mirrorIds.begin(), mode.mirrorIds.end(), mirrorConf.name) == mode.mirrorIds.end()) {
                                if (ImGui::Selectable(mirrorConf.name.c_str())) {
                                    mode.mirrorIds.push_back(mirrorConf.name);
                                    g_configIsDirty = true;
                                }
                            }
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::TreePop();
                }

                if (ImGui::TreeNode((tr("modes.mirror_groups") + "##EyeZoom").c_str())) {
                    int group_idx_to_remove = -1;
                    for (size_t k = 0; k < mode.mirrorGroupIds.size(); ++k) {
                        ImGui::PushID(static_cast<int>(k));
                        std::string del_group_label = "X##del_mirror_group_from_eyezoom_" + std::to_string(k);
                        if (ImGui::Button(del_group_label.c_str())) { group_idx_to_remove = (int)k; }
                        ImGui::SameLine();
                        ImGui::TextUnformatted(mode.mirrorGroupIds[k].c_str());
                        ImGui::PopID();
                    }
                    if (group_idx_to_remove != -1) {
                        mode.mirrorGroupIds.erase(mode.mirrorGroupIds.begin() + group_idx_to_remove);
                        g_configIsDirty = true;
                    }
                    if (ImGui::BeginCombo((tr("modes.add_mirror_group") + "##add_mirror_group_to_eyezoom").c_str(), trc("modes.select_mirror_group"))) {
                        for (const auto& groupConf : g_config.mirrorGroups) {
                            if (std::find(mode.mirrorGroupIds.begin(), mode.mirrorGroupIds.end(), groupConf.name) ==
                                mode.mirrorGroupIds.end()) {
                                if (ImGui::Selectable(groupConf.name.c_str())) {
                                    mode.mirrorGroupIds.push_back(groupConf.name);
                                    g_configIsDirty = true;
                                }
                            }
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::TreePop();
                }

                if (ImGui::TreeNode(trc("modes.eyezoom.images"))) {
                    int image_idx_to_remove = -1;
                    for (size_t k = 0; k < mode.imageIds.size(); ++k) {
                        ImGui::PushID(static_cast<int>(k));
                        std::string del_img_label = "X##del_img_from_mode_" + std::to_string(k);
                        if (ImGui::Button(del_img_label.c_str())) { image_idx_to_remove = (int)k; }
                        ImGui::SameLine();
                        ImGui::TextUnformatted(mode.imageIds[k].c_str());
                        ImGui::PopID();
                    }
                    if (image_idx_to_remove != -1) {
                        mode.imageIds.erase(mode.imageIds.begin() + image_idx_to_remove);
                        g_configIsDirty = true;
                    }
                    if (ImGui::BeginCombo((tr("modes.add_image") + "##add_image_to_eyezoom").c_str(), trc("modes.select_image"))) {
                        for (const auto& imgConf : g_config.images) {
                            if (std::find(mode.imageIds.begin(), mode.imageIds.end(), imgConf.name) == mode.imageIds.end()) {
                                if (ImGui::Selectable(imgConf.name.c_str())) {
                                    mode.imageIds.push_back(imgConf.name);
                                    g_configIsDirty = true;
                                }
                            }
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::TreePop();
                }

                if (ImGui::TreeNode(trc("modes.eyezoom.window_overlays"))) {
                    int overlay_idx_to_remove = -1;
                    for (size_t k = 0; k < mode.windowOverlayIds.size(); ++k) {
                        ImGui::PushID(static_cast<int>(k));
                        std::string del_overlay_label = "X##del_overlay_from_mode_" + std::to_string(k);
                        if (ImGui::Button(del_overlay_label.c_str())) { overlay_idx_to_remove = (int)k; }
                        ImGui::SameLine();
                        ImGui::TextUnformatted(mode.windowOverlayIds[k].c_str());
                        ImGui::PopID();
                    }
                    if (overlay_idx_to_remove != -1) {
                        mode.windowOverlayIds.erase(mode.windowOverlayIds.begin() + overlay_idx_to_remove);
                        g_configIsDirty = true;
                    }
                    if (ImGui::BeginCombo((tr("modes.add_overlay") + "##add_overlay_to_mode").c_str(), trc("modes.select_overlay"))) {
                        for (const auto& overlayConf : g_config.windowOverlays) {
                            if (std::find(mode.windowOverlayIds.begin(), mode.windowOverlayIds.end(), overlayConf.name) ==
                                mode.windowOverlayIds.end()) {
                                if (ImGui::Selectable(overlayConf.name.c_str())) {
                                    mode.windowOverlayIds.push_back(overlayConf.name);
                                    g_configIsDirty = true;
                                }
                            }
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::TreePop();
                }

                ImGui::Separator();
                if (ImGui::TreeNode((tr("modes.transition_settings") + "##EyeZoom").c_str())) {
                    RenderTransitionSettingsHorizontal(mode, "EyeZoom");

                    ImGui::Separator();
                    if (ImGui::Checkbox(trc("modes.eyezoom.slide_zoom_in"), &g_config.eyezoom.slideZoomIn)) { g_configIsDirty = true; }
                    ImGui::SameLine();
                    HelpMarker(trc("modes.eyezoom.tooltip.slide_zoom_in"));

                    if (ImGui::Checkbox(trc("modes.slide_mirrors_in"), &g_config.eyezoom.slideMirrorsIn)) { g_configIsDirty = true; }
                    ImGui::SameLine();
                    HelpMarker(trc("modes.tooltip.slide_mirrors_in"));

                    ImGui::TreePop();
                }


                if (ImGui::TreeNode((tr("modes.sensitivity_override") + "##EyeZoom").c_str())) {
                    if (ImGui::Checkbox(trc("modes.override_sensitivity"), &mode.sensitivityOverrideEnabled)) { g_configIsDirty = true; }
                    HelpMarker(trc("modes.tooltip.override_sensitivity"));

                    if (mode.sensitivityOverrideEnabled) {
                        if (ImGui::Checkbox((tr("modes.separate_xy_sensitivity") + "##EyeZoom").c_str(), &mode.separateXYSensitivity)) {
                            g_configIsDirty = true;
                            if (mode.separateXYSensitivity) {
                                mode.modeSensitivityX = mode.modeSensitivity;
                                mode.modeSensitivityY = mode.modeSensitivity;
                            }
                        }
                        ImGui::SameLine();
                        HelpMarker(trc("modes.tooltip.separate_xy_sensitivity"));

                        if (mode.separateXYSensitivity) {
                            RawInputSensitivityNote();
                            ImGui::Text(trc("modes.sensitivity_x"));
                            ImGui::SetNextItemWidth(200);
                            if (ImGui::SliderFloat("##EyeZoomSensitivityX", &mode.modeSensitivityX, 0.001f, 10.0f, "%.3fx")) {
                                g_configIsDirty = true;
                            }
                            RawInputSensitivityNote();
                            ImGui::Text(trc("modes.sensitivity_y"));
                            ImGui::SetNextItemWidth(200);
                            if (ImGui::SliderFloat("##EyeZoomSensitivityY", &mode.modeSensitivityY, 0.001f, 10.0f, "%.3fx")) {
                                g_configIsDirty = true;
                            }
                        } else {
                            RawInputSensitivityNote();
                            ImGui::Text(trc("modes.sensitivity"));
                            ImGui::SetNextItemWidth(200);
                            if (ImGui::SliderFloat("##EyeZoomSensitivity", &mode.modeSensitivity, 0.001f, 10.0f, "%.3fx")) {
                                g_configIsDirty = true;
                            }
                            ImGui::SameLine();
                            HelpMarker(trc("modes.tooltip.sensitivity"));
                        }
                    }
                    ImGui::TreePop();
                }

                ImGui::TreePop();
            }
            ImGui::PopID();
            break;
        }
    }

    for (size_t i = 0; i < g_config.modes.size(); ++i) {
        auto& mode = g_config.modes[i];
        if (EqualsIgnoreCase(mode.id, "Preemptive")) {
            ImGui::PushID((int)i + 15000);

            bool node_open = ImGui::TreeNodeEx("##mode_node", ImGuiTreeNodeFlags_SpanAvailWidth, "%s", mode.id.c_str());
            ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ImGui::GetFrameHeight());

            ImGui::Dummy(ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()));

            if (node_open) {
                if (!resolutionSupported) { ImGui::BeginDisabled(); }

                const ModeConfig* eyezoomMode = nullptr;
                for (const auto& m2 : g_config.modes) {
                    if (EqualsIgnoreCase(m2.id, "EyeZoom")) {
                        eyezoomMode = &m2;
                        break;
                    }
                }

                int copiedW = eyezoomMode ? eyezoomMode->width : mode.width;
                int copiedH = eyezoomMode ? eyezoomMode->height : mode.height;

                ImGui::Columns(2, "preemptive_dims", false);
                ImGui::SetColumnWidth(0, 150);

                ImGui::Text(trc("modes.game_width"));
                ImGui::NextColumn();
                {
                    ImGui::BeginDisabled();
                    int tempW = copiedW;
                    (void)Spinner("##PreemptiveModeWidth", &tempW, 1, 1, screenWidth);
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    ImGui::TextDisabled(trc("modes.preemptive.copied_from_eyezoom"));
                }

                ImGui::NextColumn();
                ImGui::Text(trc("modes.game_height"));
                ImGui::NextColumn();
                {
                    ImGui::BeginDisabled();
                    int tempH = copiedH;
                    (void)Spinner("##PreemptiveModeHeight", &tempH, 1, 1, 16384);
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    ImGui::TextDisabled(trc("modes.preemptive.copied_from_eyezoom"));
                }
                ImGui::Columns(1);

                if (!eyezoomMode) {
                    ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.0f, 1.0f), trc("modes.preemptive.eyezoom_not_found"));
                }

                if (ImGui::Button((tr("modes.switch_to_this_mode") + "##Preemptive").c_str())) {
                    std::lock_guard<std::mutex> pendingLock(g_pendingModeSwitchMutex);
                    g_pendingModeSwitch.pending = true;
                    g_pendingModeSwitch.modeId = mode.id;
                    g_pendingModeSwitch.source = "GUI Preemptive mode";
                }
                ImGui::SameLine();
                if (ImGui::Button((tr("modes.set_as_default") + "##Preemptive").c_str())) {
                    g_config.defaultMode = mode.id;
                    g_configIsDirty = true;
                }

                if (!resolutionSupported) { ImGui::EndDisabled(); }

                if (g_currentModeId == mode.id) {
                    ImGui::SameLine();
                    ImGui::TextDisabled(trc("label.current"));
                }

                if (ImGui::TreeNode((tr("modes.background") + "##Preemptive").c_str())) {
                    if (ImGui::RadioButton((tr("modes.color") + "##Preemptive").c_str(), mode.background.selectedMode == "color")) {
                        if (mode.background.selectedMode != "color") {
                            mode.background.selectedMode = "color";
                            g_configIsDirty = true;
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::RadioButton((tr("modes.gradient") + "##Preemptive").c_str(), mode.background.selectedMode == "gradient")) {
                        if (mode.background.selectedMode != "gradient") {
                            mode.background.selectedMode = "gradient";
                            if (mode.background.gradientStops.size() < 2) {
                                mode.background.gradientStops.clear();
                                mode.background.gradientStops.push_back({ { 0.0f, 0.0f, 0.0f }, 0.0f });
                                mode.background.gradientStops.push_back({ { 1.0f, 1.0f, 1.0f }, 1.0f });
                            }
                            g_configIsDirty = true;
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::RadioButton((tr("label.image") + "##Preemptive").c_str(), mode.background.selectedMode == "image")) {
                        if (mode.background.selectedMode != "image") {
                            mode.background.selectedMode = "image";
                            g_configIsDirty = true;
                            if (!mode.background.image.empty()) {
                                g_allImagesLoaded = false;
                                g_pendingImageLoad = true;
                                LoadImageAsync(DecodedImageData::Type::Background, mode.id, mode.background.image, g_toolscreenPath);
                            }
                        }
                    }

                    if (mode.background.selectedMode == "color") {
                        if (ImGui::ColorEdit3("##bgColorPreemptive", &mode.background.color.r)) { g_configIsDirty = true; }
                    } else if (mode.background.selectedMode == "gradient") {
                        ImGui::SetNextItemWidth(200);
                        if (ImGui::SliderFloat("Angle##bgGradAnglePreemptive", &mode.background.gradientAngle, 0.0f, 360.0f, "%.0f deg")) {
                            g_configIsDirty = true;
                        }

                        ImGui::Text(trc("modes.color_stops"));
                        int stopToRemove = -1;
                        for (size_t i2 = 0; i2 < mode.background.gradientStops.size(); i2++) {
                            ImGui::PushID(static_cast<int>(i2));
                            auto& stop = mode.background.gradientStops[i2];

                            if (ImGui::ColorEdit3("##StopColor", &stop.color.r, ImGuiColorEditFlags_NoInputs)) { g_configIsDirty = true; }
                            ImGui::SameLine();

                            float pos = stop.position * 100.0f;
                            ImGui::SetNextItemWidth(100);
                            if (ImGui::SliderFloat("##StopPos", &pos, 0.0f, 100.0f, "%.0f%%")) {
                                stop.position = pos / 100.0f;
                                g_configIsDirty = true;
                            }

                            if (mode.background.gradientStops.size() > 2) {
                                ImGui::SameLine();
                                if (ImGui::Button("X##RemoveStop")) { stopToRemove = static_cast<int>(i2); }
                            }

                            ImGui::PopID();
                        }
                        if (stopToRemove >= 0) {
                            mode.background.gradientStops.erase(mode.background.gradientStops.begin() + stopToRemove);
                            g_configIsDirty = true;
                        }

                        if (mode.background.gradientStops.size() < 8) {
                            if (ImGui::Button((tr("modes.gradient_add_color_stop") + "##bgGradPreemptive").c_str())) {
                                GradientColorStop newStop;
                                newStop.position = 0.5f;
                                newStop.color = { 0.5f, 0.5f, 0.5f };
                                mode.background.gradientStops.push_back(newStop);
                                std::sort(mode.background.gradientStops.begin(), mode.background.gradientStops.end(),
                                          [](const GradientColorStop& a, const GradientColorStop& b) { return a.position < b.position; });
                                g_configIsDirty = true;
                            }
                        }

                        ImGui::Separator();
                        ImGui::Text(trc("modes.gradient_animation"));
                        const char* animTypeNames[] = { trc("modes.gradient_animation_none"), trc("modes.gradient_animation_rotate"), trc("modes.gradient_animation_slide"), trc("modes.gradient_animation_wave"), trc("modes.gradient_animation_spiral"), trc("modes.gradient_animation_fade") };
                        int currentAnimType = static_cast<int>(mode.background.gradientAnimation);
                        ImGui::SetNextItemWidth(120);
                        if (ImGui::Combo("Type##GradAnimPreemptive", &currentAnimType, animTypeNames, IM_ARRAYSIZE(animTypeNames))) {
                            mode.background.gradientAnimation = static_cast<GradientAnimationType>(currentAnimType);
                            g_configIsDirty = true;
                        }
                        if (mode.background.gradientAnimation != GradientAnimationType::None) {
                            ImGui::SetNextItemWidth(150);
                            if (ImGui::SliderFloat("Speed##GradAnimSpeedPreemptive", &mode.background.gradientAnimationSpeed, 0.1f, 5.0f,
                                                   "%.1fx")) {
                                g_configIsDirty = true;
                            }
                        }
                    } else if (mode.background.selectedMode == "image") {
                        if (ImGui::InputText("Path##preemptive_bg", &mode.background.image)) {
                            ClearImageError("preemptive_bg");
                            g_configIsDirty = true;
                            g_allImagesLoaded = false;
                            g_pendingImageLoad = true;
                        }
                        ImGui::SameLine();
                        if (ImGui::Button((tr("button.browse") + "##preemptive_bg").c_str())) {
                            ImagePickerResult result =
                                OpenImagePickerAndValidate(g_minecraftHwnd.load(), g_toolscreenPath, g_toolscreenPath);

                            if (result.completed) {
                                if (result.success) {
                                    mode.background.image = result.path;
                                    ClearImageError("preemptive_bg");
                                    g_allImagesLoaded = false;
                                    g_pendingImageLoad = true;
                                    g_configIsDirty = true;
                                } else if (!result.error.empty()) {
                                    SetImageError("preemptive_bg", result.error);
                                }
                            }
                        }

                        std::string bgError = GetImageError("preemptive_bg");
                        if (!bgError.empty()) { ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", bgError.c_str()); }
                    }
                    ImGui::TreePop();
                }

                if (ImGui::TreeNode((tr("modes.border_settings") + "##Preemptive").c_str())) {
                    if (ImGui::Checkbox((tr("modes.enable_border") + "##Preemptive").c_str(), &mode.border.enabled)) { g_configIsDirty = true; }
                    ImGui::SameLine();
                    HelpMarker(trc("modes.tooltip.enable_border"));

                    if (mode.border.enabled) {
                        ImGui::Text(trc("images.border_color"));
                        ImVec4 borderCol = ImVec4(mode.border.color.r, mode.border.color.g, mode.border.color.b, 1.0f);
                        if (ImGui::ColorEdit3("##BorderColorPreemptive", (float*)&borderCol, ImGuiColorEditFlags_NoInputs)) {
                            mode.border.color = { borderCol.x, borderCol.y, borderCol.z };
                            g_configIsDirty = true;
                        }

                        ImGui::Text(trc("modes.border_width"));
                        ImGui::SetNextItemWidth(100);
                        if (Spinner("##BorderWidthPreemptive", &mode.border.width, 1, 1, 50)) { g_configIsDirty = true; }
                        ImGui::SameLine();
                        ImGui::TextDisabled(trc("label.px"));

                        ImGui::Text(trc("images.border_radius"));
                        ImGui::SetNextItemWidth(100);
                        if (Spinner("##BorderRadiusPreemptive", &mode.border.radius, 1, 0, 100)) { g_configIsDirty = true; }
                        ImGui::SameLine();
                        ImGui::TextDisabled(trc("label.px"));
                    }
                    ImGui::TreePop();
                }

                if (ImGui::TreeNode((tr("modes.mirrors") + "##Preemptive").c_str())) {
                    int mirror_idx_to_remove = -1;
                    for (size_t k = 0; k < mode.mirrorIds.size(); ++k) {
                        ImGui::PushID(static_cast<int>(k));
                        std::string del_mirror_label = "X##del_mirror_from_preemptive_" + std::to_string(k);
                        if (ImGui::Button(del_mirror_label.c_str())) { mirror_idx_to_remove = (int)k; }
                        ImGui::SameLine();
                        ImGui::TextUnformatted(mode.mirrorIds[k].c_str());
                        ImGui::PopID();
                    }
                    if (mirror_idx_to_remove != -1) {
                        mode.mirrorIds.erase(mode.mirrorIds.begin() + mirror_idx_to_remove);
                        g_configIsDirty = true;
                    }
                    if (ImGui::BeginCombo((tr("modes.add_mirror") + "##add_mirror_to_preemptive").c_str(), trc("modes.select_mirror"))) {
                        for (const auto& mirrorConf : g_config.mirrors) {
                            if (std::find(mode.mirrorIds.begin(), mode.mirrorIds.end(), mirrorConf.name) == mode.mirrorIds.end()) {
                                if (ImGui::Selectable(mirrorConf.name.c_str())) {
                                    mode.mirrorIds.push_back(mirrorConf.name);
                                    g_configIsDirty = true;
                                }
                            }
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::TreePop();
                }

                if (ImGui::TreeNode((tr("modes.mirror_groups") + "##Preemptive").c_str())) {
                    int group_idx_to_remove = -1;
                    for (size_t k = 0; k < mode.mirrorGroupIds.size(); ++k) {
                        ImGui::PushID(static_cast<int>(k));
                        std::string del_group_label = "X##del_mirror_group_from_preemptive_" + std::to_string(k);
                        if (ImGui::Button(del_group_label.c_str())) { group_idx_to_remove = (int)k; }
                        ImGui::SameLine();
                        ImGui::TextUnformatted(mode.mirrorGroupIds[k].c_str());
                        ImGui::PopID();
                    }
                    if (group_idx_to_remove != -1) {
                        mode.mirrorGroupIds.erase(mode.mirrorGroupIds.begin() + group_idx_to_remove);
                        g_configIsDirty = true;
                    }
                    if (ImGui::BeginCombo((tr("modes.add_mirror_group") + "##add_mirror_group_to_preemptive").c_str(), trc("modes.select_mirror_group"))) {
                        for (const auto& groupConf : g_config.mirrorGroups) {
                            if (std::find(mode.mirrorGroupIds.begin(), mode.mirrorGroupIds.end(), groupConf.name) ==
                                mode.mirrorGroupIds.end()) {
                                if (ImGui::Selectable(groupConf.name.c_str())) {
                                    mode.mirrorGroupIds.push_back(groupConf.name);
                                    g_configIsDirty = true;
                                }
                            }
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::TreePop();
                }

                if (ImGui::TreeNode((tr("modes.images") + "##Preemptive").c_str())) {
                    int image_idx_to_remove = -1;
                    for (size_t k = 0; k < mode.imageIds.size(); ++k) {
                        ImGui::PushID(static_cast<int>(k));
                        std::string del_img_label = "X##del_img_from_preemptive_" + std::to_string(k);
                        if (ImGui::Button(del_img_label.c_str())) { image_idx_to_remove = (int)k; }
                        ImGui::SameLine();
                        ImGui::TextUnformatted(mode.imageIds[k].c_str());
                        ImGui::PopID();
                    }
                    if (image_idx_to_remove != -1) {
                        mode.imageIds.erase(mode.imageIds.begin() + image_idx_to_remove);
                        g_configIsDirty = true;
                    }
                    if (ImGui::BeginCombo((tr("modes.add_image") + "##add_image_to_preemptive").c_str(), trc("modes.select_image"))) {
                        for (const auto& imgConf : g_config.images) {
                            if (std::find(mode.imageIds.begin(), mode.imageIds.end(), imgConf.name) == mode.imageIds.end()) {
                                if (ImGui::Selectable(imgConf.name.c_str())) {
                                    mode.imageIds.push_back(imgConf.name);
                                    g_configIsDirty = true;
                                }
                            }
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::TreePop();
                }

                if (ImGui::TreeNode((tr("modes.window_overlays") + "##Preemptive").c_str())) {
                    int overlay_idx_to_remove = -1;
                    for (size_t k = 0; k < mode.windowOverlayIds.size(); ++k) {
                        ImGui::PushID(static_cast<int>(k));
                        std::string del_overlay_label = "X##del_overlay_from_preemptive_" + std::to_string(k);
                        if (ImGui::Button(del_overlay_label.c_str())) { overlay_idx_to_remove = (int)k; }
                        ImGui::SameLine();
                        ImGui::TextUnformatted(mode.windowOverlayIds[k].c_str());
                        ImGui::PopID();
                    }
                    if (overlay_idx_to_remove != -1) {
                        mode.windowOverlayIds.erase(mode.windowOverlayIds.begin() + overlay_idx_to_remove);
                        g_configIsDirty = true;
                    }
                    if (ImGui::BeginCombo((tr("modes.add_overlay") + "##add_overlay_to_preemptive").c_str(), trc("modes.select_overlay"))) {
                        for (const auto& overlayConf : g_config.windowOverlays) {
                            if (std::find(mode.windowOverlayIds.begin(), mode.windowOverlayIds.end(), overlayConf.name) ==
                                mode.windowOverlayIds.end()) {
                                if (ImGui::Selectable(overlayConf.name.c_str())) {
                                    mode.windowOverlayIds.push_back(overlayConf.name);
                                    g_configIsDirty = true;
                                }
                            }
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::TreePop();
                }

                ImGui::Separator();
                if (ImGui::TreeNode((tr("modes.transition_settings") + "##Preemptive").c_str())) {
                    RenderTransitionSettingsHorizontal(mode, "Preemptive");
                    if (ImGui::Checkbox((tr("modes.slide_mirrors_in") + "##Preemptive").c_str(), &mode.slideMirrorsIn)) { g_configIsDirty = true; }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip(trc("modes.tooltip.slide_mirrors_in"));
                    }
                    ImGui::TreePop();
                }

                if (ImGui::TreeNode((tr("modes.sensitivity_override") + "##Preemptive").c_str())) {
                    if (ImGui::Checkbox((tr("modes.override_sensitivity") + "##Preemptive").c_str(), &mode.sensitivityOverrideEnabled)) { g_configIsDirty = true; }
                    HelpMarker(trc("modes.tooltip.override_sensitivity"));

                    if (mode.sensitivityOverrideEnabled) {
                        if (ImGui::Checkbox((tr("modes.separate_xy_sensitivity") + "##Preemptive").c_str(), &mode.separateXYSensitivity)) {
                            g_configIsDirty = true;
                            if (mode.separateXYSensitivity) {
                                mode.modeSensitivityX = mode.modeSensitivity;
                                mode.modeSensitivityY = mode.modeSensitivity;
                            }
                        }
                        ImGui::SameLine();
                        HelpMarker(trc("modes.tooltip.separate_xy_sensitivity"));

                        if (mode.separateXYSensitivity) {
                            RawInputSensitivityNote();
                            ImGui::Text(trc("modes.sensitivity_x"));
                            ImGui::SetNextItemWidth(200);
                            if (ImGui::SliderFloat("##PreemptiveSensitivityX", &mode.modeSensitivityX, 0.001f, 10.0f, "%.3fx")) {
                                g_configIsDirty = true;
                            }
                            RawInputSensitivityNote();
                            ImGui::Text(trc("modes.sensitivity_y"));
                            ImGui::SetNextItemWidth(200);
                            if (ImGui::SliderFloat("##PreemptiveSensitivityY", &mode.modeSensitivityY, 0.001f, 10.0f, "%.3fx")) {
                                g_configIsDirty = true;
                            }
                        } else {
                            RawInputSensitivityNote();
                            ImGui::Text(trc("modes.sensitivity"));
                            ImGui::SetNextItemWidth(200);
                            if (ImGui::SliderFloat("##PreemptiveSensitivity", &mode.modeSensitivity, 0.001f, 10.0f, "%.3fx")) {
                                g_configIsDirty = true;
                            }
                            ImGui::SameLine();
                            HelpMarker(trc("modes.tooltip.sensitivity"));
                        }
                    }
                    ImGui::TreePop();
                }

                ImGui::TreePop();
            }

            ImGui::PopID();
            break;
        }
    }

    for (size_t i = 0; i < g_config.modes.size(); ++i) {
        auto& mode = g_config.modes[i];
        if (EqualsIgnoreCase(mode.id, "Thin")) {
            ImGui::PushID((int)i + 20000);

            bool node_open = ImGui::TreeNodeEx("##mode_node", ImGuiTreeNodeFlags_SpanAvailWidth, "%s", mode.id.c_str());
            ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ImGui::GetFrameHeight());
            ImGui::Dummy(ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()));

            if (node_open) {
                if (!resolutionSupported) { ImGui::BeginDisabled(); }


                bool useManualPixelSizeThin = !mode.useRelativeSize;
                if (ImGui::Checkbox((tr("modes.label.manual_pixel_size") + "##Thin").c_str(), &useManualPixelSizeThin)) {
                    mode.useRelativeSize = !useManualPixelSizeThin;
                    if (mode.useRelativeSize) {
                        mode.widthExpr.clear();
                        mode.heightExpr.clear();
                        mode.relativeWidth = (std::max)(0.01f, (std::min)(1.0f, static_cast<float>(mode.width) / static_cast<float>(screenWidth)));
                        mode.relativeHeight =
                            (std::max)(0.01f, (std::min)(1.0f, static_cast<float>(mode.height) / static_cast<float>(screenHeight)));
                        int thinMinWidthPx = 330;
                        float thinMinWidthPct = (std::min)(100.0f, (static_cast<float>(thinMinWidthPx) / static_cast<float>(screenWidth)) * 100.0f);
                        float currentWidthPct = mode.relativeWidth * 100.0f;
                        if (currentWidthPct < thinMinWidthPct) {
                            mode.relativeWidth = thinMinWidthPct / 100.0f;
                            mode.width = (std::max)(thinMinWidthPx,
                                                    static_cast<int>(mode.relativeWidth * static_cast<float>(screenWidth)));
                        }
                    } else {
                        if (mode.width < 330) { mode.width = 330; }
                        mode.relativeWidth = -1.0f;
                        mode.relativeHeight = -1.0f;
                    }
                    g_configIsDirty = true;
                }

                ImGui::Columns(2, "thin_dims", false);

                ImGui::Text(trc("label.width"));
                ImGui::NextColumn();
                if (mode.useRelativeSize) {
                    int thinMinWidthPx = 330;
                    float thinMinWidthPct = (std::min)(100.0f, (static_cast<float>(thinMinWidthPx) / static_cast<float>(screenWidth)) * 100.0f);
                    float widthPct = ((mode.relativeWidth >= 0.0f && mode.relativeWidth <= 1.0f)
                                          ? mode.relativeWidth
                                          : (static_cast<float>(mode.width) / static_cast<float>(screenWidth))) *
                                     100.0f;
                    widthPct = (std::max)(thinMinWidthPct, (std::min)(100.0f, widthPct));
                    if (ImGui::SliderFloat("##ThinWidthPercent", &widthPct, thinMinWidthPct, 100.0f, "%.1f%%")) {
                        mode.useRelativeSize = true;
                        mode.widthExpr.clear();
                        mode.relativeWidth = widthPct / 100.0f;
                        mode.width = (std::max)(thinMinWidthPx, static_cast<int>(mode.relativeWidth * static_cast<float>(screenWidth)));
                        g_configIsDirty = true;
                        if (g_currentModeId == mode.id) {
                            HWND hwnd = g_minecraftHwnd.load();
                            if (hwnd) { PostMessage(hwnd, WM_SIZE, SIZE_RESTORED, MAKELPARAM(mode.width, mode.height)); }
                        }
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("(%d px)", mode.width);
                } else {
                    int tempWidth3 = mode.width;
                    if (Spinner("##Width", &tempWidth3, 1, 330, screenWidth)) {
                        std::lock_guard<std::mutex> lock(g_pendingDimensionChangeMutex);
                        g_pendingDimensionChange.pending = true;
                        g_pendingDimensionChange.modeId = mode.id;
                        g_pendingDimensionChange.newWidth = tempWidth3;
                        g_pendingDimensionChange.newHeight = 0;
                        g_pendingDimensionChange.sendWmSize = (g_currentModeId == mode.id);
                    }
                }
                ImGui::NextColumn();
                ImGui::Text(trc("label.height"));
                ImGui::NextColumn();
                if (mode.useRelativeSize) {
                    float heightPct = ((mode.relativeHeight >= 0.0f && mode.relativeHeight <= 1.0f)
                                           ? mode.relativeHeight
                                           : (static_cast<float>(mode.height) / static_cast<float>(screenHeight))) *
                                      100.0f;
                    heightPct = (std::max)(1.0f, (std::min)(100.0f, heightPct));
                    if (ImGui::SliderFloat("##ThinHeightPercent", &heightPct, 1.0f, 100.0f, "%.1f%%")) {
                        mode.useRelativeSize = true;
                        mode.heightExpr.clear();
                        mode.relativeHeight = heightPct / 100.0f;
                        mode.height = (std::max)(1, static_cast<int>(mode.relativeHeight * static_cast<float>(screenHeight)));
                        g_configIsDirty = true;
                        if (g_currentModeId == mode.id) {
                            HWND hwnd = g_minecraftHwnd.load();
                            if (hwnd) { PostMessage(hwnd, WM_SIZE, SIZE_RESTORED, MAKELPARAM(mode.width, mode.height)); }
                        }
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("(%d px)", mode.height);
                } else {
                    int tempHeight3 = mode.height;
                    if (Spinner("##Height", &tempHeight3, 1, 1, screenHeight)) {
                        std::lock_guard<std::mutex> lock(g_pendingDimensionChangeMutex);
                        g_pendingDimensionChange.pending = true;
                        g_pendingDimensionChange.modeId = mode.id;
                        g_pendingDimensionChange.newWidth = 0;
                        g_pendingDimensionChange.newHeight = tempHeight3;
                        g_pendingDimensionChange.sendWmSize = (g_currentModeId == mode.id);
                    }
                }
                ImGui::Columns(1);

                if (ImGui::Button((tr("modes.switch_to_this_mode") + "##Thin").c_str())) {
                    std::lock_guard<std::mutex> pendingLock(g_pendingModeSwitchMutex);
                    g_pendingModeSwitch.pending = true;
                    g_pendingModeSwitch.modeId = mode.id;
                    g_pendingModeSwitch.source = "GUI Thin mode";
                }
                ImGui::SameLine();
                if (ImGui::Button((tr("modes.set_as_default") + "##Thin").c_str())) {
                    g_config.defaultMode = mode.id;
                    g_configIsDirty = true;
                }

                if (!resolutionSupported) { ImGui::EndDisabled(); }

                if (g_currentModeId == mode.id) {
                    ImGui::SameLine();
                    ImGui::TextDisabled(trc("label.current"));
                }

                ImGui::Separator();
                if (ImGui::TreeNode(trc("modes.transition_settings"))) {
                    RenderTransitionSettingsHorizontal(mode, "Thin");
                    if (ImGui::Checkbox(trc("modes.slide_mirrors_in"), &mode.slideMirrorsIn)) { g_configIsDirty = true; }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Mirrors slide in from the screen edges instead of appearing instantly");
                    }
                    ImGui::TreePop();
                }

                if (ImGui::TreeNode(trc("modes.background"))) {
                    if (ImGui::RadioButton(trc("modes.color"), mode.background.selectedMode == "color")) {
                        mode.background.selectedMode = "color";
                        g_configIsDirty = true;
                    }
                    ImGui::SameLine();
                    if (ImGui::RadioButton(trc("modes.gradient"), mode.background.selectedMode == "gradient")) {
                        if (mode.background.selectedMode != "gradient") {
                            mode.background.selectedMode = "gradient";
                            if (mode.background.gradientStops.size() < 2) {
                                mode.background.gradientStops.clear();
                                mode.background.gradientStops.push_back({ { 0.0f, 0.0f, 0.0f }, 0.0f });
                                mode.background.gradientStops.push_back({ { 1.0f, 1.0f, 1.0f }, 1.0f });
                            }
                            g_configIsDirty = true;
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::RadioButton(trc("modes.image"), mode.background.selectedMode == "image")) {
                        if (mode.background.selectedMode != "image") {
                            mode.background.selectedMode = "image";
                            g_configIsDirty = true;
                            if (!mode.background.image.empty()) {
                                g_allImagesLoaded = false;
                                g_pendingImageLoad = true;
                                LoadImageAsync(DecodedImageData::Type::Background, mode.id, mode.background.image, g_toolscreenPath);
                            }
                        }
                    }
                    if (mode.background.selectedMode == "color") {
                        if (ImGui::ColorEdit3("##bgColorThin", &mode.background.color.r)) { g_configIsDirty = true; }
                    } else if (mode.background.selectedMode == "gradient") {
                        ImGui::SetNextItemWidth(200);
                        if (ImGui::SliderFloat((tr("modes.gradient_angle") + "##bgGradAngleThin").c_str(), &mode.background.gradientAngle, 0.0f, 360.0f, "%.0f deg")) {
                            g_configIsDirty = true;
                        }
                        ImGui::Text("Color Stops:");
                        int stopToRemove = -1;
                        for (size_t i = 0; i < mode.background.gradientStops.size(); i++) {
                            ImGui::PushID(static_cast<int>(i));
                            auto& stop = mode.background.gradientStops[i];
                            if (ImGui::ColorEdit3("##StopColor", &stop.color.r, ImGuiColorEditFlags_NoInputs)) { g_configIsDirty = true; }
                            ImGui::SameLine();
                            float pos = stop.position * 100.0f;
                            ImGui::SetNextItemWidth(100);
                            if (ImGui::SliderFloat("##StopPos", &pos, 0.0f, 100.0f, "%.0f%%")) {
                                stop.position = pos / 100.0f;
                                g_configIsDirty = true;
                            }
                            if (mode.background.gradientStops.size() > 2) {
                                ImGui::SameLine();
                                if (ImGui::Button("X##RemoveStop")) { stopToRemove = static_cast<int>(i); }
                            }
                            ImGui::PopID();
                        }
                        if (stopToRemove >= 0) {
                            mode.background.gradientStops.erase(mode.background.gradientStops.begin() + stopToRemove);
                            g_configIsDirty = true;
                        }
                        if (mode.background.gradientStops.size() < 8) {
                            if (ImGui::Button(trc("modes.gradient_add_color_stop"))) {
                                GradientColorStop newStop;
                                newStop.position = 0.5f;
                                newStop.color = { 0.5f, 0.5f, 0.5f };
                                mode.background.gradientStops.push_back(newStop);
                                std::sort(mode.background.gradientStops.begin(), mode.background.gradientStops.end(),
                                          [](const GradientColorStop& a, const GradientColorStop& b) { return a.position < b.position; });
                                g_configIsDirty = true;
                            }
                        }

                        ImGui::Separator();
                        ImGui::Text("Animation:");
                        const char* animTypeNamesThin[] = { "None", "Rotate", "Slide", "Wave", "Spiral", "Fade" };
                        int currentAnimTypeThin = static_cast<int>(mode.background.gradientAnimation);
                        ImGui::SetNextItemWidth(120);
                        if (ImGui::Combo((tr("modes.gradient_animation_type") + "##GradAnimThin").c_str(), &currentAnimTypeThin, animTypeNamesThin, IM_ARRAYSIZE(animTypeNamesThin))) {
                            mode.background.gradientAnimation = static_cast<GradientAnimationType>(currentAnimTypeThin);
                            g_configIsDirty = true;
                        }
                        if (mode.background.gradientAnimation != GradientAnimationType::None) {
                            ImGui::SetNextItemWidth(150);
                            if (ImGui::SliderFloat((tr("modes.gradient_animation_speed") + "##GradAnimSpeedThin").c_str(), &mode.background.gradientAnimationSpeed, 0.1f, 5.0f, "%.1fx")) {
                                g_configIsDirty = true;
                            }
                            /*
                            if (ImGui::Checkbox("Color Fade##GradColorFadeThin", &mode.background.gradientColorFade)) {
                                g_configIsDirty = true;
                            }
                            if (ImGui::IsItemHovered()) {
                                ImGui::SetTooltip("Smoothly cycle colors through gradient stops");
                            }*/
                        }
                    } else if (mode.background.selectedMode == "image") {
                        std::string thinErrorKey = "mode_bg_thin";
                        if (ImGui::InputText((tr("modes.bg_image_path") + "##Thin").c_str(), &mode.background.image)) {
                            ClearImageError(thinErrorKey);
                            g_configIsDirty = true;
                            g_allImagesLoaded = false;
                            g_pendingImageLoad = true;
                        }
                        ImGui::SameLine();
                        if (ImGui::Button("Browse...##thin_bg")) {
                            ImagePickerResult result =
                                OpenImagePickerAndValidate(g_minecraftHwnd.load(), g_toolscreenPath, g_toolscreenPath);
                            if (result.completed) {
                                if (result.success) {
                                    mode.background.image = result.path;
                                    ClearImageError(thinErrorKey);
                                    g_allImagesLoaded = false;
                                    g_pendingImageLoad = true;
                                    g_configIsDirty = true;
                                } else if (!result.error.empty()) {
                                    SetImageError(thinErrorKey, result.error);
                                }
                            }
                        }
                        std::string thinBgError = GetImageError(thinErrorKey);
                        if (!thinBgError.empty()) { ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", thinBgError.c_str()); }
                    }
                    ImGui::TreePop();
                }

                if (ImGui::TreeNode(trc("modes.border_settings"))) {
                    if (ImGui::Checkbox(trc("modes.enable_border"), &mode.border.enabled)) { g_configIsDirty = true; }
                    if (mode.border.enabled) {
                        ImGui::Text(trc("modes.color"));
                        ImVec4 borderCol = ImVec4(mode.border.color.r, mode.border.color.g, mode.border.color.b, 1.0f);
                        if (ImGui::ColorEdit3("##BorderColorThin", (float*)&borderCol, ImGuiColorEditFlags_NoInputs)) {
                            mode.border.color = { borderCol.x, borderCol.y, borderCol.z };
                            g_configIsDirty = true;
                        }
                        ImGui::Text(trc("images.border_width"));
                        if (Spinner("##BorderWidthThin", &mode.border.width, 1, 1, 50)) { g_configIsDirty = true; }
                    }
                    ImGui::TreePop();
                }

                if (ImGui::TreeNode(trc("modes.mirrors"))) {
                    int mirror_idx_to_remove = -1;
                    for (size_t k = 0; k < mode.mirrorIds.size(); ++k) {
                        ImGui::PushID(static_cast<int>(k));
                        if (ImGui::Button("X##del_mirror")) { mirror_idx_to_remove = (int)k; }
                        ImGui::SameLine();
                        ImGui::TextUnformatted(mode.mirrorIds[k].c_str());
                        ImGui::PopID();
                    }
                    if (mirror_idx_to_remove != -1) {
                        mode.mirrorIds.erase(mode.mirrorIds.begin() + mirror_idx_to_remove);
                        g_configIsDirty = true;
                    }
                    if (ImGui::BeginCombo("Add Mirror##Thin", "[Select Mirror]")) {
                        for (const auto& mirrorConf : g_config.mirrors) {
                            if (std::find(mode.mirrorIds.begin(), mode.mirrorIds.end(), mirrorConf.name) == mode.mirrorIds.end()) {
                                if (ImGui::Selectable(mirrorConf.name.c_str())) {
                                    mode.mirrorIds.push_back(mirrorConf.name);
                                    g_configIsDirty = true;
                                }
                            }
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::TreePop();
                }

                if (ImGui::TreeNode(trc("modes.mirror_groups"))) {
                    int group_idx_to_remove = -1;
                    for (size_t k = 0; k < mode.mirrorGroupIds.size(); ++k) {
                        ImGui::PushID(static_cast<int>(k));
                        std::string del_group_label = "X##del_mirror_group_from_thin_" + std::to_string(k);
                        if (ImGui::Button(del_group_label.c_str())) { group_idx_to_remove = (int)k; }
                        ImGui::SameLine();
                        ImGui::TextUnformatted(mode.mirrorGroupIds[k].c_str());
                        ImGui::PopID();
                    }
                    if (group_idx_to_remove != -1) {
                        mode.mirrorGroupIds.erase(mode.mirrorGroupIds.begin() + group_idx_to_remove);
                        g_configIsDirty = true;
                    }
                    if (ImGui::BeginCombo((tr("modes.add_mirror_group") + "##add_mirror_group_to_thin").c_str(), trc("modes.select_mirror_group"))) {
                        for (const auto& groupConf : g_config.mirrorGroups) {
                            if (std::find(mode.mirrorGroupIds.begin(), mode.mirrorGroupIds.end(), groupConf.name) ==
                                mode.mirrorGroupIds.end()) {
                                if (ImGui::Selectable(groupConf.name.c_str())) {
                                    mode.mirrorGroupIds.push_back(groupConf.name);
                                    g_configIsDirty = true;
                                }
                            }
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::TreePop();
                }

                if (ImGui::TreeNode(trc("modes.images"))) {
                    int image_idx_to_remove = -1;
                    for (size_t k = 0; k < mode.imageIds.size(); ++k) {
                        ImGui::PushID(static_cast<int>(k));
                        if (ImGui::Button("X##del_img")) { image_idx_to_remove = (int)k; }
                        ImGui::SameLine();
                        ImGui::TextUnformatted(mode.imageIds[k].c_str());
                        ImGui::PopID();
                    }
                    if (image_idx_to_remove != -1) {
                        mode.imageIds.erase(mode.imageIds.begin() + image_idx_to_remove);
                        g_configIsDirty = true;
                    }
                    if (ImGui::BeginCombo((tr("modes.add_image") + "##Thin").c_str(), trc("modes.select_image"))) {
                        for (const auto& imgConf : g_config.images) {
                            if (std::find(mode.imageIds.begin(), mode.imageIds.end(), imgConf.name) == mode.imageIds.end()) {
                                if (ImGui::Selectable(imgConf.name.c_str())) {
                                    mode.imageIds.push_back(imgConf.name);
                                    g_configIsDirty = true;
                                }
                            }
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::TreePop();
                }

                if (ImGui::TreeNode(trc("modes.window_overlays"))) {
                    int windowOverlay_idx_to_remove = -1;
                    for (size_t k = 0; k < mode.windowOverlayIds.size(); ++k) {
                        ImGui::PushID(static_cast<int>(k));
                        if (ImGui::Button("X##del_window_overlay")) { windowOverlay_idx_to_remove = (int)k; }
                        ImGui::SameLine();
                        ImGui::TextUnformatted(mode.windowOverlayIds[k].c_str());
                        ImGui::PopID();
                    }
                    if (windowOverlay_idx_to_remove != -1) {
                        mode.windowOverlayIds.erase(mode.windowOverlayIds.begin() + windowOverlay_idx_to_remove);
                        g_configIsDirty = true;
                    }
                    if (ImGui::BeginCombo((tr("modes.add_overlay") + "##add_window_overlay_to_thin").c_str(), trc("modes.select_overlay"))) {
                        for (const auto& overlayConf : g_config.windowOverlays) {
                            if (std::find(mode.windowOverlayIds.begin(), mode.windowOverlayIds.end(), overlayConf.name) ==
                                mode.windowOverlayIds.end()) {
                                if (ImGui::Selectable(overlayConf.name.c_str())) {
                                    mode.windowOverlayIds.push_back(overlayConf.name);
                                    g_configIsDirty = true;
                                }
                            }
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::TreePop();
                }

                if (ImGui::TreeNode(trc("modes.sensitivity_override"))) {
                    if (ImGui::Checkbox(trc("modes.override_sensitivity"), &mode.sensitivityOverrideEnabled)) { g_configIsDirty = true; }
                    HelpMarker("When enabled, this mode uses its own mouse sensitivity instead of the global setting.");

                    if (mode.sensitivityOverrideEnabled) {
                        if (ImGui::Checkbox(trc("modes.separate_xy_sensitivity"), &mode.separateXYSensitivity)) {
                            g_configIsDirty = true;
                            if (mode.separateXYSensitivity) {
                                mode.modeSensitivityX = mode.modeSensitivity;
                                mode.modeSensitivityY = mode.modeSensitivity;
                            }
                        }
                        ImGui::SameLine();
                        HelpMarker("Use different sensitivity values for horizontal (X) and vertical (Y) mouse movement.");

                        if (mode.separateXYSensitivity) {
                            RawInputSensitivityNote();
                            ImGui::Text("X Sensitivity:");
                            ImGui::SetNextItemWidth(200);
                            if (ImGui::SliderFloat("##ThinSensitivityX", &mode.modeSensitivityX, 0.001f, 10.0f, "%.3fx")) {
                                g_configIsDirty = true;
                            }
                            RawInputSensitivityNote();
                            ImGui::Text("Y Sensitivity:");
                            ImGui::SetNextItemWidth(200);
                            if (ImGui::SliderFloat("##ThinSensitivityY", &mode.modeSensitivityY, 0.001f, 10.0f, "%.3fx")) {
                                g_configIsDirty = true;
                            }
                        } else {
                            RawInputSensitivityNote();
                            ImGui::Text("Sensitivity:");
                            ImGui::SetNextItemWidth(200);
                            if (ImGui::SliderFloat("##ThinSensitivity", &mode.modeSensitivity, 0.001f, 10.0f, "%.3fx")) {
                                g_configIsDirty = true;
                            }
                            ImGui::SameLine();
                            HelpMarker("Mouse sensitivity for this mode (1.0 = normal)");
                        }
                    }
                    ImGui::TreePop();
                }

                ImGui::TreePop();
            }
            ImGui::PopID();
            break;
        }
    }

    for (size_t i = 0; i < g_config.modes.size(); ++i) {
        auto& mode = g_config.modes[i];
        if (EqualsIgnoreCase(mode.id, "Wide")) {
            ImGui::PushID((int)i + 30000);

            bool node_open = ImGui::TreeNodeEx("##mode_node", ImGuiTreeNodeFlags_SpanAvailWidth, "%s", mode.id.c_str());
            ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ImGui::GetFrameHeight());
            ImGui::Dummy(ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()));

            if (node_open) {
                if (!resolutionSupported) { ImGui::BeginDisabled(); }

                bool useManualPixelSizeWide = !mode.useRelativeSize;
                if (ImGui::Checkbox((tr("modes.label.manual_pixel_size") + "##Wide").c_str(), &useManualPixelSizeWide)) {
                    mode.useRelativeSize = !useManualPixelSizeWide;
                    if (mode.useRelativeSize) {
                        mode.widthExpr.clear();
                        mode.heightExpr.clear();
                        mode.relativeWidth = (std::max)(0.01f, (std::min)(1.0f, static_cast<float>(mode.width) / static_cast<float>(screenWidth)));
                        mode.relativeHeight =
                            (std::max)(0.01f, (std::min)(1.0f, static_cast<float>(mode.height) / static_cast<float>(screenHeight)));
                    } else {
                        mode.relativeWidth = -1.0f;
                        mode.relativeHeight = -1.0f;
                    }
                    g_configIsDirty = true;
                }

                ImGui::Columns(2, "wide_dims", false);

                ImGui::Text(trc("label.width"));
                ImGui::NextColumn();
                if (mode.useRelativeSize) {
                    float widthPct = ((mode.relativeWidth >= 0.0f && mode.relativeWidth <= 1.0f)
                                          ? mode.relativeWidth
                                          : (static_cast<float>(mode.width) / static_cast<float>(screenWidth))) *
                                     100.0f;
                    widthPct = (std::max)(1.0f, (std::min)(100.0f, widthPct));
                    if (ImGui::SliderFloat("##WideWidthPercent", &widthPct, 1.0f, 100.0f, "%.1f%%")) {
                        mode.useRelativeSize = true;
                        mode.widthExpr.clear();
                        mode.relativeWidth = widthPct / 100.0f;
                        mode.width = (std::max)(1, static_cast<int>(mode.relativeWidth * static_cast<float>(screenWidth)));
                        g_configIsDirty = true;
                        if (g_currentModeId == mode.id) {
                            HWND hwnd = g_minecraftHwnd.load();
                            if (hwnd) { PostMessage(hwnd, WM_SIZE, SIZE_RESTORED, MAKELPARAM(mode.width, mode.height)); }
                        }
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("(%d px)", mode.width);
                } else {
                    int tempWidth4 = mode.width;
                    if (Spinner("##Width", &tempWidth4, 1, 1, screenWidth)) {
                        std::lock_guard<std::mutex> lock(g_pendingDimensionChangeMutex);
                        g_pendingDimensionChange.pending = true;
                        g_pendingDimensionChange.modeId = mode.id;
                        g_pendingDimensionChange.newWidth = tempWidth4;
                        g_pendingDimensionChange.newHeight = 0;
                        g_pendingDimensionChange.sendWmSize = (g_currentModeId == mode.id);
                    }
                }
                ImGui::NextColumn();
                ImGui::Text(trc("label.height"));
                ImGui::NextColumn();
                if (mode.useRelativeSize) {
                    float heightPct = ((mode.relativeHeight >= 0.0f && mode.relativeHeight <= 1.0f)
                                           ? mode.relativeHeight
                                           : (static_cast<float>(mode.height) / static_cast<float>(screenHeight))) *
                                      100.0f;
                    heightPct = (std::max)(1.0f, (std::min)(100.0f, heightPct));
                    if (ImGui::SliderFloat("##WideHeightPercent", &heightPct, 1.0f, 100.0f, "%.1f%%")) {
                        mode.useRelativeSize = true;
                        mode.heightExpr.clear();
                        mode.relativeHeight = heightPct / 100.0f;
                        mode.height = (std::max)(1, static_cast<int>(mode.relativeHeight * static_cast<float>(screenHeight)));
                        g_configIsDirty = true;
                        if (g_currentModeId == mode.id) {
                            HWND hwnd = g_minecraftHwnd.load();
                            if (hwnd) { PostMessage(hwnd, WM_SIZE, SIZE_RESTORED, MAKELPARAM(mode.width, mode.height)); }
                        }
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("(%d px)", mode.height);
                } else {
                    int tempHeight4 = mode.height;
                    if (Spinner("##Height", &tempHeight4, 1, 1, screenHeight)) {
                        std::lock_guard<std::mutex> lock(g_pendingDimensionChangeMutex);
                        g_pendingDimensionChange.pending = true;
                        g_pendingDimensionChange.modeId = mode.id;
                        g_pendingDimensionChange.newWidth = 0;
                        g_pendingDimensionChange.newHeight = tempHeight4;
                        g_pendingDimensionChange.sendWmSize = (g_currentModeId == mode.id);
                    }
                }
                ImGui::Columns(1);

                if (ImGui::Button((tr("modes.switch_to_this_mode") + "##Wide").c_str())) {
                    std::lock_guard<std::mutex> pendingLock(g_pendingModeSwitchMutex);
                    g_pendingModeSwitch.pending = true;
                    g_pendingModeSwitch.modeId = mode.id;
                    g_pendingModeSwitch.source = "GUI Wide mode";
                }
                ImGui::SameLine();
                if (ImGui::Button((tr("modes.set_as_default") + "##Wide").c_str())) {
                    g_config.defaultMode = mode.id;
                    g_configIsDirty = true;
                }

                if (!resolutionSupported) { ImGui::EndDisabled(); }

                if (g_currentModeId == mode.id) {
                    ImGui::SameLine();
                    ImGui::TextDisabled(trc("label.current"));
                }

                ImGui::Separator();
                if (ImGui::TreeNode(trc("modes.transition_settings"))) {
                    RenderTransitionSettingsHorizontal(mode, "Wide");
                    if (ImGui::Checkbox(trc("modes.slide_mirrors_in"), &mode.slideMirrorsIn)) { g_configIsDirty = true; }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Mirrors slide in from the screen edges instead of appearing instantly");
                    }
                    ImGui::TreePop();
                }

                if (ImGui::TreeNode(trc("modes.background"))) {
                    if (ImGui::RadioButton(trc("modes.color"), mode.background.selectedMode == "color")) {
                        mode.background.selectedMode = "color";
                        g_configIsDirty = true;
                    }
                    ImGui::SameLine();
                    if (ImGui::RadioButton(trc("modes.gradient"), mode.background.selectedMode == "gradient")) {
                        if (mode.background.selectedMode != "gradient") {
                            mode.background.selectedMode = "gradient";
                            if (mode.background.gradientStops.size() < 2) {
                                mode.background.gradientStops.clear();
                                mode.background.gradientStops.push_back({ { 0.0f, 0.0f, 0.0f }, 0.0f });
                                mode.background.gradientStops.push_back({ { 1.0f, 1.0f, 1.0f }, 1.0f });
                            }
                            g_configIsDirty = true;
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::RadioButton(trc("modes.image"), mode.background.selectedMode == "image")) {
                        if (mode.background.selectedMode != "image") {
                            mode.background.selectedMode = "image";
                            g_configIsDirty = true;
                            if (!mode.background.image.empty()) {
                                g_allImagesLoaded = false;
                                g_pendingImageLoad = true;
                                LoadImageAsync(DecodedImageData::Type::Background, mode.id, mode.background.image, g_toolscreenPath);
                            }
                        }
                    }
                    if (mode.background.selectedMode == "color") {
                        if (ImGui::ColorEdit3("##bgColorWide", &mode.background.color.r)) { g_configIsDirty = true; }
                    } else if (mode.background.selectedMode == "gradient") {
                        ImGui::SetNextItemWidth(200);
                        if (ImGui::SliderFloat((tr("modes.gradient_angle") + "##bgGradAngleWide").c_str(), &mode.background.gradientAngle, 0.0f, 360.0f, "%.0f deg")) {
                            g_configIsDirty = true;
                        }
                        ImGui::Text("Color Stops:");
                        int stopToRemove = -1;
                        for (size_t i = 0; i < mode.background.gradientStops.size(); i++) {
                            ImGui::PushID(static_cast<int>(i));
                            auto& stop = mode.background.gradientStops[i];
                            if (ImGui::ColorEdit3("##StopColor", &stop.color.r, ImGuiColorEditFlags_NoInputs)) { g_configIsDirty = true; }
                            ImGui::SameLine();
                            float pos = stop.position * 100.0f;
                            ImGui::SetNextItemWidth(100);
                            if (ImGui::SliderFloat("##StopPos", &pos, 0.0f, 100.0f, "%.0f%%")) {
                                stop.position = pos / 100.0f;
                                g_configIsDirty = true;
                            }
                            if (mode.background.gradientStops.size() > 2) {
                                ImGui::SameLine();
                                if (ImGui::Button("X##RemoveStop")) { stopToRemove = static_cast<int>(i); }
                            }
                            ImGui::PopID();
                        }
                        if (stopToRemove >= 0) {
                            mode.background.gradientStops.erase(mode.background.gradientStops.begin() + stopToRemove);
                            g_configIsDirty = true;
                        }
                        if (mode.background.gradientStops.size() < 8) {
                            if (ImGui::Button(trc("modes.gradient_add_color_stop"))) {
                                GradientColorStop newStop;
                                newStop.position = 0.5f;
                                newStop.color = { 0.5f, 0.5f, 0.5f };
                                mode.background.gradientStops.push_back(newStop);
                                std::sort(mode.background.gradientStops.begin(), mode.background.gradientStops.end(),
                                          [](const GradientColorStop& a, const GradientColorStop& b) { return a.position < b.position; });
                                g_configIsDirty = true;
                            }
                        }

                        ImGui::Separator();
                        ImGui::Text("Animation:");
                        const char* animTypeNamesWide[] = { "None", "Rotate", "Slide", "Wave", "Spiral", "Fade" };
                        int currentAnimTypeWide = static_cast<int>(mode.background.gradientAnimation);
                        ImGui::SetNextItemWidth(120);
                        if (ImGui::Combo((tr("modes.gradient_animation_type") + "##GradAnimWide").c_str(), &currentAnimTypeWide, animTypeNamesWide, IM_ARRAYSIZE(animTypeNamesWide))) {
                            mode.background.gradientAnimation = static_cast<GradientAnimationType>(currentAnimTypeWide);
                            g_configIsDirty = true;
                        }
                        if (mode.background.gradientAnimation != GradientAnimationType::None) {
                            ImGui::SetNextItemWidth(150);
                            if (ImGui::SliderFloat((tr("modes.gradient_animation_speed") + "##GradAnimSpeedWide").c_str(), &mode.background.gradientAnimationSpeed, 0.1f, 5.0f, "%.1fx")) {
                                g_configIsDirty = true;
                            }
                            /*
                            if (ImGui::Checkbox("Color Fade##GradColorFadeWide", &mode.background.gradientColorFade)) {
                                g_configIsDirty = true;
                            }
                            if (ImGui::IsItemHovered()) {
                                ImGui::SetTooltip("Smoothly cycle colors through gradient stops");
                            }*/
                        }
                    } else if (mode.background.selectedMode == "image") {
                        std::string wideErrorKey = "mode_bg_wide";
                        if (ImGui::InputText((tr("modes.bg_image_path") + "##Wide").c_str(), &mode.background.image)) {
                            ClearImageError(wideErrorKey);
                            g_configIsDirty = true;
                            g_allImagesLoaded = false;
                            g_pendingImageLoad = true;
                        }
                        ImGui::SameLine();
                        if (ImGui::Button("Browse...##wide_bg")) {
                            ImagePickerResult result =
                                OpenImagePickerAndValidate(g_minecraftHwnd.load(), g_toolscreenPath, g_toolscreenPath);
                            if (result.completed) {
                                if (result.success) {
                                    mode.background.image = result.path;
                                    ClearImageError(wideErrorKey);
                                    g_allImagesLoaded = false;
                                    g_pendingImageLoad = true;
                                    g_configIsDirty = true;
                                } else if (!result.error.empty()) {
                                    SetImageError(wideErrorKey, result.error);
                                }
                            }
                        }
                        std::string wideBgError = GetImageError(wideErrorKey);
                        if (!wideBgError.empty()) { ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", wideBgError.c_str()); }
                    }
                    ImGui::TreePop();
                }

                if (ImGui::TreeNode(trc("modes.border_settings"))) {
                    if (ImGui::Checkbox(trc("modes.enable_border"), &mode.border.enabled)) { g_configIsDirty = true; }
                    if (mode.border.enabled) {
                        ImGui::Text(trc("modes.color"));
                        ImVec4 borderCol = ImVec4(mode.border.color.r, mode.border.color.g, mode.border.color.b, 1.0f);
                        if (ImGui::ColorEdit3("##BorderColorWide", (float*)&borderCol, ImGuiColorEditFlags_NoInputs)) {
                            mode.border.color = { borderCol.x, borderCol.y, borderCol.z };
                            g_configIsDirty = true;
                        }
                        ImGui::Text(trc("images.border_width"));
                        if (Spinner("##BorderWidthWide", &mode.border.width, 1, 1, 50)) { g_configIsDirty = true; }
                    }
                    ImGui::TreePop();
                }

                if (ImGui::TreeNode(trc("modes.mirrors"))) {
                    int mirror_idx_to_remove = -1;
                    for (size_t k = 0; k < mode.mirrorIds.size(); ++k) {
                        ImGui::PushID(static_cast<int>(k));
                        if (ImGui::Button("X##del_mirror")) { mirror_idx_to_remove = (int)k; }
                        ImGui::SameLine();
                        ImGui::TextUnformatted(mode.mirrorIds[k].c_str());
                        ImGui::PopID();
                    }
                    if (mirror_idx_to_remove != -1) {
                        mode.mirrorIds.erase(mode.mirrorIds.begin() + mirror_idx_to_remove);
                        g_configIsDirty = true;
                    }
                    if (ImGui::BeginCombo((tr("modes.add_mirror") + "##Wide").c_str(), trc("modes.select_mirror"))) {
                        for (const auto& mirrorConf : g_config.mirrors) {
                            if (std::find(mode.mirrorIds.begin(), mode.mirrorIds.end(), mirrorConf.name) == mode.mirrorIds.end()) {
                                if (ImGui::Selectable(mirrorConf.name.c_str())) {
                                    mode.mirrorIds.push_back(mirrorConf.name);
                                    g_configIsDirty = true;
                                }
                            }
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::TreePop();
                }

                if (ImGui::TreeNode(trc("modes.mirror_groups"))) {
                    int group_idx_to_remove = -1;
                    for (size_t k = 0; k < mode.mirrorGroupIds.size(); ++k) {
                        ImGui::PushID(static_cast<int>(k));
                        std::string del_group_label = "X##del_mirror_group_from_wide_" + std::to_string(k);
                        if (ImGui::Button(del_group_label.c_str())) { group_idx_to_remove = (int)k; }
                        ImGui::SameLine();
                        ImGui::TextUnformatted(mode.mirrorGroupIds[k].c_str());
                        ImGui::PopID();
                    }
                    if (group_idx_to_remove != -1) {
                        mode.mirrorGroupIds.erase(mode.mirrorGroupIds.begin() + group_idx_to_remove);
                        g_configIsDirty = true;
                    }
                    if (ImGui::BeginCombo((tr("modes.add_mirror_group") + "##add_mirror_group_to_wide").c_str(), trc("modes.select_mirror_group"))) {
                        for (const auto& groupConf : g_config.mirrorGroups) {
                            if (std::find(mode.mirrorGroupIds.begin(), mode.mirrorGroupIds.end(), groupConf.name) ==
                                mode.mirrorGroupIds.end()) {
                                if (ImGui::Selectable(groupConf.name.c_str())) {
                                    mode.mirrorGroupIds.push_back(groupConf.name);
                                    g_configIsDirty = true;
                                }
                            }
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::TreePop();
                }

                if (ImGui::TreeNode(trc("modes.images"))) {
                    int image_idx_to_remove = -1;
                    for (size_t k = 0; k < mode.imageIds.size(); ++k) {
                        ImGui::PushID(static_cast<int>(k));
                        if (ImGui::Button("X##del_img")) { image_idx_to_remove = (int)k; }
                        ImGui::SameLine();
                        ImGui::TextUnformatted(mode.imageIds[k].c_str());
                        ImGui::PopID();
                    }
                    if (image_idx_to_remove != -1) {
                        mode.imageIds.erase(mode.imageIds.begin() + image_idx_to_remove);
                        g_configIsDirty = true;
                    }
                    if (ImGui::BeginCombo((tr("modes.add_image") + "##Wide").c_str(), trc("modes.select_image"))) {
                        for (const auto& imgConf : g_config.images) {
                            if (std::find(mode.imageIds.begin(), mode.imageIds.end(), imgConf.name) == mode.imageIds.end()) {
                                if (ImGui::Selectable(imgConf.name.c_str())) {
                                    mode.imageIds.push_back(imgConf.name);
                                    g_configIsDirty = true;
                                }
                            }
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::TreePop();
                }

                if (ImGui::TreeNode(trc("modes.window_overlays"))) {
                    int windowOverlay_idx_to_remove = -1;
                    for (size_t k = 0; k < mode.windowOverlayIds.size(); ++k) {
                        ImGui::PushID(static_cast<int>(k));
                        if (ImGui::Button("X##del_window_overlay")) { windowOverlay_idx_to_remove = (int)k; }
                        ImGui::SameLine();
                        ImGui::TextUnformatted(mode.windowOverlayIds[k].c_str());
                        ImGui::PopID();
                    }
                    if (windowOverlay_idx_to_remove != -1) {
                        mode.windowOverlayIds.erase(mode.windowOverlayIds.begin() + windowOverlay_idx_to_remove);
                        g_configIsDirty = true;
                    }
                    if (ImGui::BeginCombo((tr("modes.add_overlay") + "##add_window_overlay_to_wide").c_str(), trc("modes.select_overlay"))) {
                        for (const auto& overlayConf : g_config.windowOverlays) {
                            if (std::find(mode.windowOverlayIds.begin(), mode.windowOverlayIds.end(), overlayConf.name) ==
                                mode.windowOverlayIds.end()) {
                                if (ImGui::Selectable(overlayConf.name.c_str())) {
                                    mode.windowOverlayIds.push_back(overlayConf.name);
                                    g_configIsDirty = true;
                                }
                            }
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::TreePop();
                }

                if (ImGui::TreeNode(trc("modes.sensitivity_override"))) {
                    if (ImGui::Checkbox(trc("modes.override_sensitivity"), &mode.sensitivityOverrideEnabled)) { g_configIsDirty = true; }
                    HelpMarker("When enabled, this mode uses its own mouse sensitivity instead of the global setting.");

                    if (mode.sensitivityOverrideEnabled) {
                        if (ImGui::Checkbox(trc("modes.separate_xy_sensitivity"), &mode.separateXYSensitivity)) {
                            g_configIsDirty = true;
                            if (mode.separateXYSensitivity) {
                                mode.modeSensitivityX = mode.modeSensitivity;
                                mode.modeSensitivityY = mode.modeSensitivity;
                            }
                        }
                        ImGui::SameLine();
                        HelpMarker("Use different sensitivity values for horizontal (X) and vertical (Y) mouse movement.");

                        if (mode.separateXYSensitivity) {
                            RawInputSensitivityNote();
                            ImGui::Text("X Sensitivity:");
                            ImGui::SetNextItemWidth(200);
                            if (ImGui::SliderFloat("##WideSensitivityX", &mode.modeSensitivityX, 0.001f, 10.0f, "%.3fx")) {
                                g_configIsDirty = true;
                            }
                            RawInputSensitivityNote();
                            ImGui::Text("Y Sensitivity:");
                            ImGui::SetNextItemWidth(200);
                            if (ImGui::SliderFloat("##WideSensitivityY", &mode.modeSensitivityY, 0.001f, 10.0f, "%.3fx")) {
                                g_configIsDirty = true;
                            }
                        } else {
                            RawInputSensitivityNote();
                            ImGui::Text("Sensitivity:");
                            ImGui::SetNextItemWidth(200);
                            if (ImGui::SliderFloat("##WideSensitivity", &mode.modeSensitivity, 0.001f, 10.0f, "%.3fx")) {
                                g_configIsDirty = true;
                            }
                            ImGui::SameLine();
                            HelpMarker("Mouse sensitivity for this mode (1.0 = normal)");
                        }
                    }
                    ImGui::TreePop();
                }

                ImGui::TreePop();
            }
            ImGui::PopID();
            break;
        }
    }

    ImGui::SeparatorText(trc("modes.custom_modes"));

    for (size_t i = 0; i < g_config.modes.size(); ++i) {
        auto& mode = g_config.modes[i];
        if (!IsHardcodedMode(mode.id)) {
            ImGui::PushID((int)i);

            if (!resolutionSupported) { ImGui::BeginDisabled(); }

            std::string delete_button_label = "X##delete_mode_" + std::to_string(i);
            if (ImGui::Button(delete_button_label.c_str(), ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()))) {
                std::string popup_id = (tr("modes.delete_mode") + "##" + std::to_string(i));
                ImGui::OpenPopup(popup_id.c_str());
            }

            if (!resolutionSupported) { ImGui::EndDisabled(); }

            std::string popup_id = (tr("modes.delete_mode") + "##" + std::to_string(i));
            if (ImGui::BeginPopupModal(popup_id.c_str(), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::Text("%s", tr("modes.delete_mode_confirm", mode.id.c_str()).c_str());
                ImGui::Separator();
                if (ImGui::Button(trc("button.ok"), ImVec2(120, 0))) {
                    mode_to_remove = (int)i;
                    g_configIsDirty = true;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SetItemDefaultFocus();
                ImGui::SameLine();
                if (ImGui::Button(trc("button.cancel"), ImVec2(120, 0))) { ImGui::CloseCurrentPopup(); }
                ImGui::EndPopup();
            }

            ImGui::SameLine();
            bool node_open = ImGui::TreeNodeEx("##mode_node", ImGuiTreeNodeFlags_SpanAvailWidth, "%s", mode.id.c_str());

            if (node_open) {
                ImGui::Text(trc("label.name"));
                ImGui::SetNextItemWidth(250);

                bool hasDuplicate = HasDuplicateModeName(mode.id, i);
                bool isReservedName = IsHardcodedMode(mode.id);
                bool hasError = hasDuplicate || isReservedName;

                if (hasError) {
                    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.6f, 0.2f, 0.2f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.7f, 0.3f, 0.3f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.8f, 0.3f, 0.3f, 1.0f));
                }

                std::string oldModeId = mode.id;
                if (ImGui::InputText("##Name", &mode.id)) {
                    bool newIsReserved = IsHardcodedMode(mode.id);
                    if (!HasDuplicateModeName(mode.id, i) && !newIsReserved) {
                        g_configIsDirty = true;
                    } else {
                        mode.id = oldModeId;
                    }
                }

                if (hasError) { ImGui::PopStyleColor(3); }

                if (hasDuplicate) {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Name already exists!");
                } else if (isReservedName) {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Name is reserved!");
                }

                if (!resolutionSupported) { ImGui::BeginDisabled(); }

                int modeScreenW = GetCachedWindowWidth();
                int modeScreenH = GetCachedWindowHeight();
                if (modeScreenW < 1) modeScreenW = 1;
                if (modeScreenH < 1) modeScreenH = 1;

                bool useManualPixelSize = !mode.useRelativeSize;
                if (ImGui::Checkbox((tr("modes.label.manual_pixel_size") + "##CustomMode").c_str(), &useManualPixelSize)) {
                    mode.useRelativeSize = !useManualPixelSize;

                    if (mode.useRelativeSize) {
                        mode.widthExpr.clear();
                        mode.heightExpr.clear();

                        float computedRelativeWidth = static_cast<float>(mode.width) / static_cast<float>(modeScreenW);
                        float computedRelativeHeight = static_cast<float>(mode.height) / static_cast<float>(modeScreenH);

                        mode.relativeWidth = (std::max)(0.01f, (std::min)(1.0f, computedRelativeWidth));
                        mode.relativeHeight = (std::max)(0.01f, (std::min)(1.0f, computedRelativeHeight));

                        int newWidth = static_cast<int>(mode.relativeWidth * static_cast<float>(modeScreenW));
                        int newHeight = static_cast<int>(mode.relativeHeight * static_cast<float>(modeScreenH));
                        if (newWidth < 1) newWidth = 1;
                        if (newHeight < 1) newHeight = 1;
                        mode.width = newWidth;
                        mode.height = newHeight;
                    } else {
                        mode.relativeWidth = -1.0f;
                        mode.relativeHeight = -1.0f;
                    }

                    g_configIsDirty = true;
                }
                ImGui::SameLine();
                HelpMarker("Off = mode size is entered as a % of current game window size.\nOn = mode size is entered directly in pixels.");

                ImGui::Columns(2, "dims", false);

                ImGui::Text(trc("label.width"));
                ImGui::NextColumn();
                if (mode.useRelativeSize) {
                    float widthPct = ((mode.relativeWidth >= 0.0f && mode.relativeWidth <= 1.0f)
                                          ? mode.relativeWidth
                                          : (static_cast<float>(mode.width) / static_cast<float>(modeScreenW))) *
                                     100.0f;
                    widthPct = (std::max)(1.0f, (std::min)(100.0f, widthPct));

                    if (ImGui::SliderFloat("##WidthPercent", &widthPct, 1.0f, 100.0f, "%.1f%%")) {
                        mode.useRelativeSize = true;
                        mode.widthExpr.clear();
                        mode.relativeWidth = widthPct / 100.0f;

                        int newWidth = static_cast<int>(mode.relativeWidth * static_cast<float>(modeScreenW));
                        if (newWidth < 1) newWidth = 1;
                        mode.width = newWidth;
                        g_configIsDirty = true;

                        if (g_currentModeId == mode.id) {
                            HWND hwnd = g_minecraftHwnd.load();
                            if (hwnd) { PostMessage(hwnd, WM_SIZE, SIZE_RESTORED, MAKELPARAM(mode.width, mode.height)); }
                        }
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("(%d px)", mode.width);
                } else {
                    int tempWidth5 = mode.width;
                    if (Spinner("##Width", &tempWidth5, 1, 1)) {
                        std::lock_guard<std::mutex> lock(g_pendingDimensionChangeMutex);
                        g_pendingDimensionChange.pending = true;
                        g_pendingDimensionChange.modeId = mode.id;
                        g_pendingDimensionChange.newWidth = tempWidth5;
                        g_pendingDimensionChange.newHeight = 0;
                        g_pendingDimensionChange.sendWmSize = (g_currentModeId == mode.id);
                    }
                }

                ImGui::NextColumn();
                ImGui::Text(trc("label.height"));
                ImGui::NextColumn();
                if (mode.useRelativeSize) {
                    float heightPct = ((mode.relativeHeight >= 0.0f && mode.relativeHeight <= 1.0f)
                                           ? mode.relativeHeight
                                           : (static_cast<float>(mode.height) / static_cast<float>(modeScreenH))) *
                                      100.0f;
                    heightPct = (std::max)(1.0f, (std::min)(100.0f, heightPct));

                    if (ImGui::SliderFloat("##HeightPercent", &heightPct, 1.0f, 100.0f, "%.1f%%")) {
                        mode.useRelativeSize = true;
                        mode.heightExpr.clear();
                        mode.relativeHeight = heightPct / 100.0f;

                        int newHeight = static_cast<int>(mode.relativeHeight * static_cast<float>(modeScreenH));
                        if (newHeight < 1) newHeight = 1;
                        mode.height = newHeight;
                        g_configIsDirty = true;

                        if (g_currentModeId == mode.id) {
                            HWND hwnd = g_minecraftHwnd.load();
                            if (hwnd) { PostMessage(hwnd, WM_SIZE, SIZE_RESTORED, MAKELPARAM(mode.width, mode.height)); }
                        }
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("(%d px)", mode.height);
                } else {
                    int tempHeight5 = mode.height;
                    if (Spinner("##Height", &tempHeight5, 1, 1)) {
                        std::lock_guard<std::mutex> lock(g_pendingDimensionChangeMutex);
                        g_pendingDimensionChange.pending = true;
                        g_pendingDimensionChange.modeId = mode.id;
                        g_pendingDimensionChange.newWidth = 0;
                        g_pendingDimensionChange.newHeight = tempHeight5;
                        g_pendingDimensionChange.sendWmSize = (g_currentModeId == mode.id);
                    }
                }
                ImGui::Columns(1);

                if (ImGui::Button(trc("modes.switch_to_this_mode"))) {
                    // Defer mode switch to avoid deadlock (g_configMutex is held during GUI rendering)
                    std::lock_guard<std::mutex> pendingLock(g_pendingModeSwitchMutex);
                    g_pendingModeSwitch.pending = true;
                    g_pendingModeSwitch.modeId = mode.id;
                    g_pendingModeSwitch.source = "GUI mode detail";
                    Log("[GUI] Deferred mode switch to: " + mode.id);
                }
                ImGui::SameLine();
                if (ImGui::Button(trc("modes.set_as_default"))) {
                    g_config.defaultMode = mode.id;
                    g_configIsDirty = true;
                }

                ImGui::Separator();
                if (ImGui::TreeNode(trc("modes.transition_settings"))) {
                    RenderTransitionSettingsHorizontal(mode, "CustomMode");
                    if (ImGui::Checkbox(trc("modes.slide_mirrors_in"), &mode.slideMirrorsIn)) { g_configIsDirty = true; }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Mirrors slide in from the screen edges instead of appearing instantly");
                    }
                    ImGui::TreePop();
                }
                ImGui::Separator();
                ;

                if (ImGui::TreeNode(trc("modes.border_settings"))) {
                    if (ImGui::Checkbox(trc("modes.enable_border"), &mode.border.enabled)) { g_configIsDirty = true; }
                    ImGui::SameLine();
                    HelpMarker("Draw a border around the game viewport. Border appears outside the game area.");

                    if (mode.border.enabled) {
                        ImGui::Text(trc("modes.color"));
                        ImVec4 borderCol = ImVec4(mode.border.color.r, mode.border.color.g, mode.border.color.b, 1.0f);
                        if (ImGui::ColorEdit3("##BorderColorCustom", (float*)&borderCol, ImGuiColorEditFlags_NoInputs)) {
                            mode.border.color = { borderCol.x, borderCol.y, borderCol.z };
                            g_configIsDirty = true;
                        }

                        ImGui::Text(trc("images.border_width"));
                        ImGui::SetNextItemWidth(100);
                        if (Spinner("##BorderWidthCustom", &mode.border.width, 1, 1, 50)) { g_configIsDirty = true; }
                        ImGui::SameLine();
                        ImGui::TextDisabled(trc("label.px"));

                        ImGui::Text(trc("images.border_radius"));
                        ImGui::SetNextItemWidth(100);
                        if (Spinner("##BorderRadiusCustom", &mode.border.radius, 1, 0, 100)) { g_configIsDirty = true; }
                        ImGui::SameLine();
                        ImGui::TextDisabled(trc("label.px"));
                    }
                    ImGui::TreePop();
                }
                ImGui::Separator();

                if (ImGui::TreeNode(trc("modes.background"))) {
                    if (ImGui::RadioButton(trc("modes.color"), mode.background.selectedMode == "color")) {
                        if (mode.background.selectedMode != "color") {
                            mode.background.selectedMode = "color";
                            g_configIsDirty = true;
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::RadioButton(trc("modes.gradient"), mode.background.selectedMode == "gradient")) {
                        if (mode.background.selectedMode != "gradient") {
                            mode.background.selectedMode = "gradient";
                            if (mode.background.gradientStops.size() < 2) {
                                mode.background.gradientStops.clear();
                                mode.background.gradientStops.push_back({ { 0.0f, 0.0f, 0.0f }, 0.0f });
                                mode.background.gradientStops.push_back({ { 1.0f, 1.0f, 1.0f }, 1.0f });
                            }
                            g_configIsDirty = true;
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::RadioButton(trc("modes.image"), mode.background.selectedMode == "image")) {
                        if (mode.background.selectedMode != "image") {
                            mode.background.selectedMode = "image";
                            g_configIsDirty = true;
                            if (!mode.background.image.empty()) {
                                g_allImagesLoaded = false;
                                g_pendingImageLoad = true;
                                LoadImageAsync(DecodedImageData::Type::Background, mode.id, mode.background.image, g_toolscreenPath);
                            }
                        }
                    }

                    if (mode.background.selectedMode == "color") {
                        if (ImGui::ColorEdit3("##bgColor", &mode.background.color.r)) { g_configIsDirty = true; }
                    } else if (mode.background.selectedMode == "gradient") {
                        std::string gradId = "##bgGrad_" + mode.id;
                        ImGui::SetNextItemWidth(200);
                        if (ImGui::SliderFloat((tr("modes.gradient_angle") + gradId).c_str(), &mode.background.gradientAngle, 0.0f, 360.0f, "%.0f deg")) {
                            g_configIsDirty = true;
                        }
                        ImGui::Text("Color Stops:");
                        int stopToRemove = -1;
                        for (size_t i = 0; i < mode.background.gradientStops.size(); i++) {
                            ImGui::PushID(static_cast<int>(i));
                            auto& stop = mode.background.gradientStops[i];
                            if (ImGui::ColorEdit3("##StopColor", &stop.color.r, ImGuiColorEditFlags_NoInputs)) { g_configIsDirty = true; }
                            ImGui::SameLine();
                            float pos = stop.position * 100.0f;
                            ImGui::SetNextItemWidth(100);
                            if (ImGui::SliderFloat("##StopPos", &pos, 0.0f, 100.0f, "%.0f%%")) {
                                stop.position = pos / 100.0f;
                                g_configIsDirty = true;
                            }
                            if (mode.background.gradientStops.size() > 2) {
                                ImGui::SameLine();
                                if (ImGui::Button("X##RemoveStop")) { stopToRemove = static_cast<int>(i); }
                            }
                            ImGui::PopID();
                        }
                        if (stopToRemove >= 0) {
                            mode.background.gradientStops.erase(mode.background.gradientStops.begin() + stopToRemove);
                            g_configIsDirty = true;
                        }
                        if (mode.background.gradientStops.size() < 8) {
                            if (ImGui::Button((tr("modes.gradient_add_color_stop") + gradId).c_str())) {
                                GradientColorStop newStop;
                                newStop.position = 0.5f;
                                newStop.color = { 0.5f, 0.5f, 0.5f };
                                mode.background.gradientStops.push_back(newStop);
                                std::sort(mode.background.gradientStops.begin(), mode.background.gradientStops.end(),
                                          [](const GradientColorStop& a, const GradientColorStop& b) { return a.position < b.position; });
                                g_configIsDirty = true;
                            }
                        }

                        ImGui::Separator();
                        ImGui::Text("Animation:");
                        const char* animTypeNamesCustom[] = { "None", "Rotate", "Slide", "Wave", "Spiral", "Fade" };
                        int currentAnimTypeCustom = static_cast<int>(mode.background.gradientAnimation);
                        ImGui::SetNextItemWidth(120);
                        if (ImGui::Combo((tr("modes.gradient_animation_type") + gradId).c_str(), &currentAnimTypeCustom, animTypeNamesCustom, IM_ARRAYSIZE(animTypeNamesCustom))) {
                            mode.background.gradientAnimation = static_cast<GradientAnimationType>(currentAnimTypeCustom);
                            g_configIsDirty = true;
                        }
                        if (mode.background.gradientAnimation != GradientAnimationType::None) {
                            ImGui::SetNextItemWidth(150);
                            if (ImGui::SliderFloat((tr("modes.gradient_animation_speed") + gradId).c_str(), &mode.background.gradientAnimationSpeed, 0.1f, 5.0f, "%.1fx")) {
                                g_configIsDirty = true;
                            }
                            /*
                            if (ImGui::Checkbox(("Color Fade" + gradId).c_str(), &mode.background.gradientColorFade)) {
                                g_configIsDirty = true;
                            }
                            if (ImGui::IsItemHovered()) {
                                ImGui::SetTooltip("Smoothly cycle colors through gradient stops");
                            }*/
                        }
                    } else if (mode.background.selectedMode == "image") {
                        std::string modeErrorKey = "mode_bg_" + mode.id;
                        if (ImGui::InputText(trc("modes.bg_image_path"), &mode.background.image)) {
                            ClearImageError(modeErrorKey);
                            g_configIsDirty = true;
                            g_allImagesLoaded = false;
                            g_pendingImageLoad = true;
                        }
                        ImGui::SameLine();
                        if (ImGui::Button(("Browse...##mode_bg_" + mode.id).c_str())) {
                            ImagePickerResult result =
                                OpenImagePickerAndValidate(g_minecraftHwnd.load(), g_toolscreenPath, g_toolscreenPath);

                            if (result.completed) {
                                if (result.success) {
                                    mode.background.image = result.path;
                                    ClearImageError(modeErrorKey);
                                    g_allImagesLoaded = false;
                                    g_pendingImageLoad = true;
                                    g_configIsDirty = true;
                                } else if (!result.error.empty()) {
                                    SetImageError(modeErrorKey, result.error);
                                }
                            }
                        }

                        std::string bgError = GetImageError(modeErrorKey);
                        if (!bgError.empty()) { ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", bgError.c_str()); }
                    }
                    ImGui::TreePop();
                }

                if (ImGui::TreeNode(trc("modes.mirrors"))) {
                    int mirror_idx_to_remove = -1;
                    for (size_t k = 0; k < mode.mirrorIds.size(); ++k) {
                        ImGui::PushID(static_cast<int>(k));
                        std::string del_mirror_label = "X##del_mirror_from_mode_" + std::to_string(k);
                        if (ImGui::Button(del_mirror_label.c_str())) { mirror_idx_to_remove = (int)k; }
                        ImGui::SameLine();
                        ImGui::TextUnformatted(mode.mirrorIds[k].c_str());
                        ImGui::PopID();
                    }
                    if (mirror_idx_to_remove != -1) {
                        mode.mirrorIds.erase(mode.mirrorIds.begin() + mirror_idx_to_remove);
                        g_configIsDirty = true;
                    }
                    if (ImGui::BeginCombo((tr("modes.add_mirror") + "##add_mirror_to_mode").c_str(), trc("modes.select_mirror"))) {
                        for (const auto& mirrorConf : g_config.mirrors) {
                            if (std::find(mode.mirrorIds.begin(), mode.mirrorIds.end(), mirrorConf.name) == mode.mirrorIds.end()) {
                                if (ImGui::Selectable(mirrorConf.name.c_str())) {
                                    mode.mirrorIds.push_back(mirrorConf.name);
                                    g_configIsDirty = true;
                                }
                            }
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::TreePop();
                }

                if (ImGui::TreeNode(trc("modes.mirror_groups"))) {
                    int group_idx_to_remove = -1;
                    for (size_t k = 0; k < mode.mirrorGroupIds.size(); ++k) {
                        ImGui::PushID(static_cast<int>(k));
                        std::string del_group_label = "X##del_mirror_group_from_custom_" + std::to_string(k);
                        if (ImGui::Button(del_group_label.c_str())) { group_idx_to_remove = (int)k; }
                        ImGui::SameLine();
                        ImGui::TextUnformatted(mode.mirrorGroupIds[k].c_str());
                        ImGui::PopID();
                    }
                    if (group_idx_to_remove != -1) {
                        mode.mirrorGroupIds.erase(mode.mirrorGroupIds.begin() + group_idx_to_remove);
                        g_configIsDirty = true;
                    }
                    if (ImGui::BeginCombo((tr("modes.add_mirror_group") + "##add_mirror_group_to_custom").c_str(), trc("modes.select_mirror_group"))) {
                        for (const auto& groupConf : g_config.mirrorGroups) {
                            if (std::find(mode.mirrorGroupIds.begin(), mode.mirrorGroupIds.end(), groupConf.name) ==
                                mode.mirrorGroupIds.end()) {
                                if (ImGui::Selectable(groupConf.name.c_str())) {
                                    mode.mirrorGroupIds.push_back(groupConf.name);
                                    g_configIsDirty = true;
                                }
                            }
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::TreePop();
                }

                if (ImGui::TreeNode(trc("modes.images"))) {
                    int image_idx_to_remove = -1;
                    for (size_t k = 0; k < mode.imageIds.size(); ++k) {
                        ImGui::PushID(static_cast<int>(k));
                        std::string del_img_label = "X##del_img_from_mode_" + std::to_string(k);
                        if (ImGui::Button(del_img_label.c_str())) { image_idx_to_remove = (int)k; }
                        ImGui::SameLine();
                        ImGui::TextUnformatted(mode.imageIds[k].c_str());
                        ImGui::PopID();
                    }
                    if (image_idx_to_remove != -1) {
                        mode.imageIds.erase(mode.imageIds.begin() + image_idx_to_remove);
                        g_configIsDirty = true;
                    }
                    if (ImGui::BeginCombo((tr("modes.add_image") + "##add_image_to_mode").c_str(), trc("modes.select_image"))) {
                        for (const auto& imgConf : g_config.images) {
                            if (std::find(mode.imageIds.begin(), mode.imageIds.end(), imgConf.name) == mode.imageIds.end()) {
                                if (ImGui::Selectable(imgConf.name.c_str())) {
                                    mode.imageIds.push_back(imgConf.name);
                                    g_configIsDirty = true;
                                }
                            }
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::TreePop();
                }

                if (ImGui::TreeNode(trc("modes.window_overlays"))) {
                    int windowOverlay_idx_to_remove = -1;
                    for (size_t k = 0; k < mode.windowOverlayIds.size(); ++k) {
                        ImGui::PushID(static_cast<int>(k));
                        std::string del_overlay_label = "X##del_overlay_from_mode_" + std::to_string(k);
                        if (ImGui::Button(del_overlay_label.c_str())) { windowOverlay_idx_to_remove = (int)k; }
                        ImGui::SameLine();
                        ImGui::TextUnformatted(mode.windowOverlayIds[k].c_str());
                        ImGui::PopID();
                    }
                    if (windowOverlay_idx_to_remove != -1) {
                        mode.windowOverlayIds.erase(mode.windowOverlayIds.begin() + windowOverlay_idx_to_remove);
                        g_configIsDirty = true;
                    }
                    if (ImGui::BeginCombo((tr("modes.add_overlay") + "##add_overlay_to_mode").c_str(), trc("modes.select_overlay"))) {
                        for (const auto& overlayConf : g_config.windowOverlays) {
                            if (std::find(mode.windowOverlayIds.begin(), mode.windowOverlayIds.end(), overlayConf.name) ==
                                mode.windowOverlayIds.end()) {
                                if (ImGui::Selectable(overlayConf.name.c_str())) {
                                    mode.windowOverlayIds.push_back(overlayConf.name);
                                    g_configIsDirty = true;
                                }
                            }
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::TreePop();
                }

                if (ImGui::TreeNode("Stretch Properties")) {
                    if (ImGui::Checkbox("Enable Stretch", &mode.stretch.enabled)) g_configIsDirty = true;
                    ImGui::Columns(2, "stretch_cols", false);
                    ImGui::SetColumnWidth(0, 150);
                    ImGui::Text("X Position");
                    ImGui::NextColumn();
                    if (Spinner("##StretchX", &mode.stretch.x)) g_configIsDirty = true;
                    ImGui::SameLine();
                    if (ImGui::Button("Center H")) {
                        mode.stretch.x = (GetCachedWindowWidth() - mode.stretch.width) / 2;
                        g_configIsDirty = true;
                    }
                    ImGui::NextColumn();
                    ImGui::Text("Width");
                    ImGui::NextColumn();
                    if (Spinner("##StretchW", &mode.stretch.width, 1, 1)) g_configIsDirty = true;
                    ImGui::NextColumn();
                    ImGui::Text("Y Position");
                    ImGui::NextColumn();
                    if (Spinner("##StretchY", &mode.stretch.y)) g_configIsDirty = true;
                    ImGui::SameLine();
                    if (ImGui::Button("Center V")) {
                        mode.stretch.y = (GetCachedWindowHeight() - mode.stretch.height) / 2;
                        g_configIsDirty = true;
                    }
                    ImGui::NextColumn();
                    ImGui::Text("Height");
                    ImGui::NextColumn();
                    if (Spinner("##StretchH", &mode.stretch.height, 1, 1)) g_configIsDirty = true;
                    ImGui::Columns(1);
                    ImGui::TreePop();
                }

                if (ImGui::TreeNode("Expressions")) {
                    ImGui::TextWrapped("Use expressions for dynamic dimensions based on screen size.");
                    ImGui::TextDisabled(trc("label.variables"));
                    ImGui::TextDisabled(trc("label.functions"));
                    ImGui::Separator();

                    int screenW = GetCachedWindowWidth();
                    int screenH = GetCachedWindowHeight();

                    ImGui::Text("Mode Width:");
                    ImGui::SetNextItemWidth(250);
                    if (ImGui::InputText("##ModeWidthExpr", &mode.widthExpr)) {
                        g_configIsDirty = true;
                        if (!mode.widthExpr.empty()) {
                            int val = EvaluateExpression(mode.widthExpr, screenW, screenH, mode.width);
                            if (val > 0) mode.width = val;
                        }
                    }
                    if (!mode.widthExpr.empty()) {
                        std::string err;
                        if (!ValidateExpression(mode.widthExpr, err)) {
                            ImGui::SameLine();
                            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Invalid");
                            if (ImGui::IsItemHovered()) { ImGui::SetTooltip("%s", err.c_str()); }
                        } else {
                            ImGui::SameLine();
                            ImGui::TextDisabled(trc("label.equals_format", mode.width));
                        }
                    }

                    ImGui::Text("Mode Height:");
                    ImGui::SetNextItemWidth(250);
                    if (ImGui::InputText("##ModeHeightExpr", &mode.heightExpr)) {
                        g_configIsDirty = true;
                        if (!mode.heightExpr.empty()) {
                            int val = EvaluateExpression(mode.heightExpr, screenW, screenH, mode.height);
                            if (val > 0) mode.height = val;
                        }
                    }
                    if (!mode.heightExpr.empty()) {
                        std::string err;
                        if (!ValidateExpression(mode.heightExpr, err)) {
                            ImGui::SameLine();
                            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Invalid");
                            if (ImGui::IsItemHovered()) { ImGui::SetTooltip("%s", err.c_str()); }
                        } else {
                            ImGui::SameLine();
                            ImGui::TextDisabled(trc("label.equals_format", mode.height));
                        }
                    }

                    ImGui::Separator();
                    ImGui::Text("Stretch Expressions:");

                    ImGui::Text("Stretch Width:");
                    ImGui::SetNextItemWidth(250);
                    if (ImGui::InputText("##StretchWidthExpr", &mode.stretch.widthExpr)) {
                        g_configIsDirty = true;
                        if (!mode.stretch.widthExpr.empty()) {
                            int val = EvaluateExpression(mode.stretch.widthExpr, screenW, screenH, mode.stretch.width);
                            if (val >= 0) mode.stretch.width = val;
                        }
                    }
                    if (!mode.stretch.widthExpr.empty()) {
                        std::string err;
                        if (!ValidateExpression(mode.stretch.widthExpr, err)) {
                            ImGui::SameLine();
                            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Invalid");
                            if (ImGui::IsItemHovered()) { ImGui::SetTooltip("%s", err.c_str()); }
                        } else {
                            ImGui::SameLine();
                            ImGui::TextDisabled(trc("label.equals_format", mode.stretch.width));
                        }
                    }

                    ImGui::Text("Stretch Height:");
                    ImGui::SetNextItemWidth(250);
                    if (ImGui::InputText("##StretchHeightExpr", &mode.stretch.heightExpr)) {
                        g_configIsDirty = true;
                        if (!mode.stretch.heightExpr.empty()) {
                            int val = EvaluateExpression(mode.stretch.heightExpr, screenW, screenH, mode.stretch.height);
                            if (val >= 0) mode.stretch.height = val;
                        }
                    }
                    if (!mode.stretch.heightExpr.empty()) {
                        std::string err;
                        if (!ValidateExpression(mode.stretch.heightExpr, err)) {
                            ImGui::SameLine();
                            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Invalid");
                            if (ImGui::IsItemHovered()) { ImGui::SetTooltip("%s", err.c_str()); }
                        } else {
                            ImGui::SameLine();
                            ImGui::TextDisabled(trc("label.equals_format", mode.stretch.height));
                        }
                    }

                    ImGui::Text("Stretch X Position:");
                    ImGui::SetNextItemWidth(250);
                    if (ImGui::InputText("##StretchXExpr", &mode.stretch.xExpr)) {
                        g_configIsDirty = true;
                        if (!mode.stretch.xExpr.empty()) {
                            mode.stretch.x = EvaluateExpression(mode.stretch.xExpr, screenW, screenH, mode.stretch.x);
                        }
                    }
                    if (!mode.stretch.xExpr.empty()) {
                        std::string err;
                        if (!ValidateExpression(mode.stretch.xExpr, err)) {
                            ImGui::SameLine();
                            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Invalid");
                            if (ImGui::IsItemHovered()) { ImGui::SetTooltip("%s", err.c_str()); }
                        } else {
                            ImGui::SameLine();
                            ImGui::TextDisabled(trc("label.equals_format", mode.stretch.x));
                        }
                    }

                    ImGui::Text("Stretch Y Position:");
                    ImGui::SetNextItemWidth(250);
                    if (ImGui::InputText("##StretchYExpr", &mode.stretch.yExpr)) {
                        g_configIsDirty = true;
                        if (!mode.stretch.yExpr.empty()) {
                            mode.stretch.y = EvaluateExpression(mode.stretch.yExpr, screenW, screenH, mode.stretch.y);
                        }
                    }
                    if (!mode.stretch.yExpr.empty()) {
                        std::string err;
                        if (!ValidateExpression(mode.stretch.yExpr, err)) {
                            ImGui::SameLine();
                            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Invalid");
                            if (ImGui::IsItemHovered()) { ImGui::SetTooltip("%s", err.c_str()); }
                        } else {
                            ImGui::SameLine();
                            ImGui::TextDisabled(trc("label.equals_format", mode.stretch.y));
                        }
                    }

                    ImGui::TreePop();
                }

                if (ImGui::TreeNode(trc("modes.sensitivity_override"))) {
                    if (ImGui::Checkbox(trc("modes.override_sensitivity"), &mode.sensitivityOverrideEnabled)) { g_configIsDirty = true; }
                    HelpMarker("When enabled, this mode uses its own mouse sensitivity instead of the global setting.");

                    if (mode.sensitivityOverrideEnabled) {
                        if (ImGui::Checkbox(trc("modes.separate_xy_sensitivity"), &mode.separateXYSensitivity)) {
                            g_configIsDirty = true;
                            if (mode.separateXYSensitivity) {
                                mode.modeSensitivityX = mode.modeSensitivity;
                                mode.modeSensitivityY = mode.modeSensitivity;
                            }
                        }
                        ImGui::SameLine();
                        HelpMarker("Use different sensitivity values for horizontal (X) and vertical (Y) mouse movement.");

                        if (mode.separateXYSensitivity) {
                            RawInputSensitivityNote();
                            ImGui::Text("X Sensitivity:");
                            ImGui::SetNextItemWidth(200);
                            if (ImGui::SliderFloat("##ModeSensitivityX", &mode.modeSensitivityX, 0.001f, 10.0f, "%.3fx")) {
                                g_configIsDirty = true;
                            }
                            RawInputSensitivityNote();
                            ImGui::Text("Y Sensitivity:");
                            ImGui::SetNextItemWidth(200);
                            if (ImGui::SliderFloat("##ModeSensitivityY", &mode.modeSensitivityY, 0.001f, 10.0f, "%.3fx")) {
                                g_configIsDirty = true;
                            }
                        } else {
                            RawInputSensitivityNote();
                            ImGui::Text("Sensitivity:");
                            ImGui::SetNextItemWidth(200);
                            if (ImGui::SliderFloat("##ModeSensitivity", &mode.modeSensitivity, 0.001f, 10.0f, "%.3fx")) {
                                g_configIsDirty = true;
                            }
                            ImGui::SameLine();
                            HelpMarker("Mouse sensitivity for this mode (1.0 = normal)");
                        }
                    }
                    ImGui::TreePop();
                }

                if (!resolutionSupported) { ImGui::EndDisabled(); }

                ImGui::TreePop();
            }
            ImGui::PopID();
        }
    }
    if (mode_to_remove != -1) {
        auto& modeToDelete = g_config.modes[mode_to_remove];
        if (!IsHardcodedMode(modeToDelete.id)) {
            std::string currentMode;
            {
                std::lock_guard<std::mutex> modeLock(g_modeIdMutex);
                currentMode = g_currentModeId;
            }
            if (EqualsIgnoreCase(currentMode, modeToDelete.id)) {
                std::string fallbackMode = (EqualsIgnoreCase(g_config.defaultMode, modeToDelete.id) || g_config.defaultMode.empty())
                                               ? "Fullscreen" : g_config.defaultMode;
                std::lock_guard<std::mutex> pendingLock(g_pendingModeSwitchMutex);
                g_pendingModeSwitch.pending = true;
                g_pendingModeSwitch.modeId = fallbackMode;
                g_pendingModeSwitch.source = "Mode deleted";
                g_pendingModeSwitch.isPreview = false;
                g_pendingModeSwitch.forceInstant = true;
                Log("[GUI] Mode '" + modeToDelete.id + "' was active and is being deleted - switching to " + fallbackMode);
            }
            if (EqualsIgnoreCase(g_config.defaultMode, modeToDelete.id)) {
                g_config.defaultMode = "Fullscreen";
            }
            g_config.modes.erase(g_config.modes.begin() + mode_to_remove);
            g_configIsDirty = true;
        }
    }

    ImGui::Separator();

    if (!resolutionSupported) { ImGui::BeginDisabled(); }

    if (ImGui::Button(trc("modes.add_new_mode"))) {
        ModeConfig newMode;
        newMode.id = "New Mode " + std::to_string(g_config.modes.size() + 1);
        newMode.useRelativeSize = true;
        newMode.relativeWidth = 1.0f;
        newMode.relativeHeight = 1.0f;
        newMode.width = GetCachedWindowWidth();
        newMode.height = GetCachedWindowHeight();
        newMode.manualWidth = newMode.width;
        newMode.manualHeight = newMode.height;
        newMode.stretch.width = 300;
        newMode.stretch.height = GetCachedWindowHeight();
        g_config.modes.push_back(newMode);
        g_configIsDirty = true;
    }

    ImGui::SameLine();
    if (ImGui::Button((tr("button.reset_defaults") + "##modes").c_str())) { ImGui::OpenPopup(trc("modes.reset_to_defaults")); }

    if (!resolutionSupported) { ImGui::EndDisabled(); }

    if (ImGui::BeginPopupModal(trc("modes.reset_to_defaults"), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.0f, 1.0f), "WARNING:");
        ImGui::Text("This will delete ALL user-created modes and restore the default modes.");
        ImGui::Text("This action cannot be undone.");
        ImGui::Separator();
        if (ImGui::Button("Confirm Reset", ImVec2(120, 0))) {
            g_config.modes = GetDefaultModes();
            g_config.eyezoom = GetDefaultEyeZoomConfig();

            // (Example: Wide mode uses height = 0.25, which must be converted to pixels.)
            int screenW = GetCachedWindowWidth();
            int screenH = GetCachedWindowHeight();
            if (screenW < 1) screenW = 1;
            if (screenH < 1) screenH = 1;

            for (auto& mode : g_config.modes) {
                bool widthIsRelative = mode.widthExpr.empty() && mode.relativeWidth >= 0.0f && mode.relativeWidth <= 1.0f;
                bool heightIsRelative = mode.heightExpr.empty() && mode.relativeHeight >= 0.0f && mode.relativeHeight <= 1.0f;

                if (widthIsRelative) {
                    int w = static_cast<int>(mode.relativeWidth * screenW);
                    if (w < 1) w = 1;
                    mode.width = w;
                }
                if (heightIsRelative) {
                    int h = static_cast<int>(mode.relativeHeight * screenH);
                    if (h < 1) h = 1;
                    mode.height = h;
                }
            }

            RecalculateExpressionDimensions();

            g_configIsDirty = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SetItemDefaultFocus();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) { ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }

    ImGui::EndTabItem();
}


