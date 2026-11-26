// settings_menu.inc - the options menu: every setting in one full-screen menu drawn like the game's own (and like the
// online menu, netplay/menu_kit.h), worked with a controller, the keyboard or the mouse. F10, a controller's Home
// (guide) button, or Back+Start together open it. Included by settings_overlay.cpp inside its anonymous namespace, so it
// works on the same state as the rest of the overlay.

namespace MK = MenuKit;

struct MenuRow {
    std::string label, value, hint;
    std::function<void(int)> change;  // -1/+1 from left/right, 0 from A/Enter or a click
    bool enabled = true;
    int bar = -1;  // 0..100: drawn as a slider
    // A slider set straight to a position (0..100) by a click or a drag; the bool: let go, so save it.
    std::function<void(int, bool)> setBar;
    bool action = false;    // a button whatever its value says: A runs it, left and right don't change it
    bool centered = false;  // a slider whose middle is its zero (brightness): filled from the middle
};

enum MenuTab { kTabResume, kTabGraphics, kTabAudio, kTabControls, kTabGameplay, kTabCheats, kTabMods, kTabOnline,
               kTabSystem, kTabQuit, kTabCount };
constexpr const char* kTabNames[kTabCount] = {"RESUME", "GRAPHICS", "AUDIO", "CONTROLS", "GAMEPLAY", "CHEATS", "MODS",
                                              "ONLINE", "SYSTEM", "QUIT GAME"};
constexpr int kVisibleRows = 8;

struct MenuState {
    int tab = kTabGraphics;
    bool inRows = false;
    int row = 0;
    // The list scrolls on its own: the mouse wheel moves the list, not the choice (Sarah's board, card 797), and a row
    // chosen with the keyboard or a controller is brought into view (follow). scroll is the top row shown, in rows.
    float scroll = 0, scrollTarget = 0;
    int scrollTab = -1;
    bool dragging = false;  // the scroll bar's thumb is held with the mouse
    float dragOffset = 0;   // from the mouse to the thumb's top, in layout units
    bool follow = false;
    bool mouse = false;       // the mouse was used last: the row under it is the chosen one
    bool escapeHeld = false;  // the Escape that opened the menu is not also Back, until ImGui has seen it go up
    bool escapeSeenDown = false;
    double escapeAt = 0;
    bool quitArmed = false;
    bool startHeld = false;  // the START held to open the menu is still down: not a press in it
    int sliderRow = -1;      // the row whose slider the mouse holds
    std::string flash;  // a line over the footer for a few seconds
    double flashUntil = 0;
    std::string pendingMod;  // a folder picked in the system's dialog, copied onto the SD card next frame
    std::mutex pendingMutex;
    std::atomic<int> modCopy{-1};  // files copied while a mod is added, -1 when idle
    std::string modResult;
    bool restartNeeded = false;
} g_menu;

int Cycle(int value, int step, int count) { return ((value + (step == 0 ? 1 : step)) % count + count) % count; }
const char* OnOff(bool on) { return on ? "ON" : "OFF"; }
void Flash(std::string text) {
    g_menu.flash = std::move(text);
    g_menu.flashUntil = ImGui::GetTime() + 4.0;
}
void OpenFolder(const std::filesystem::path& folder) {
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    std::string url = "file:///" + RuntimeConfigFile::PathToUtf8(folder);
    std::replace(url.begin(), url.end(), '\\', '/');
    if (!SDL_OpenURL(url.c_str())) Flash("Couldn't open " + RuntimeConfigFile::PathToUtf8(folder));
}

#include "settings_video.inc"  // quality presets, anti-aliasing, V-Sync's automatic setting, picture adjustments

// ---- the rows of each tab ----
void GraphicsRows(std::vector<MenuRow>& rows) {
    static constexpr const char* kModes[] = {"WINDOW", "BORDERLESS", "FULLSCREEN"};
    g_displayMode = static_cast<int>(aurora_get_display_mode());
    rows.push_back({"Display", kModes[std::clamp(g_displayMode, 0, 2)], "F11 also switches between a window and fullscreen.",
                    [](int step) {
                        aurora_set_display_mode(static_cast<AuroraDisplayMode>(Cycle(g_displayMode, step, 3)));
                        g_displayMode = static_cast<int>(aurora_get_display_mode());
                        RuntimeConfigFile::SetDisplayMode(std::string(kDisplayModeConfigNames[static_cast<size_t>(g_displayMode)]));
                    }});
    QualityRow(rows);
    int res = 0;
    for (size_t i = 0; i < kResolutions.size(); ++i) if (std::fabs(kResolutions[i].scale - g_resolutionScale) < 0.001f) res = static_cast<int>(i);
    uint32_t pw = 0, ph = 0;
    AuroraGetPictureSize(&pw, &ph);
    rows.push_back({"Resolution", kResolutions[static_cast<size_t>(res)].label,
                    "The game renders at " + std::to_string(pw) + "x" + std::to_string(ph) + ". Higher is sharper and needs a faster GPU.",
                    [res](int step) {
                        int i = res;
                        do i = Cycle(i, step, static_cast<int>(kResolutions.size()));
                        while (IsHighFrameRateMode() && IsHighResolutionScale(kResolutions[static_cast<size_t>(i)].scale));
                        SetResolutionScale(kResolutions[static_cast<size_t>(i)].scale);
                    }});
    AntiAliasingRows(rows);
    const DisplaySettings::Settings display = DisplaySettings::Latest();
    int aspect = 0;
    for (size_t i = 0; i < kAspects.size(); ++i) if (kAspects[i].mode == display.aspect) aspect = static_cast<int>(i);
    const bool restart = DisplaySettings::AspectNeedsRestart(display.aspect);
    rows.push_back({"Aspect ratio", kAspects[static_cast<size_t>(aspect)].label,
                    restart ? "Switching between 4:3 and widescreen takes effect after a restart."
                            : "Fill Window uses the whole screen, ultrawide included (up to 32:9): more of the stage to the sides, the HUD kept in the middle 16:9, never stretched.",
                    [aspect](int step) { SetAspectMode(kAspects[static_cast<size_t>(Cycle(aspect, step, static_cast<int>(kAspects.size())))].mode); }});
    static constexpr const char* kFps[] = {"60 FPS", "120 FPS", "180 FPS", "240 FPS"};
    rows.push_back({"Frame rate", kFps[g_frameInterpolationMode],
                    "Above 60 adds frames in between for screens faster than 60 Hz (experimental). The game itself runs at 60.",
                    [](int step) {
                        g_frameInterpolationMode = Cycle(g_frameInterpolationMode, step, 4);
                        const uint32_t target = kFrameInterpolationTargetFps[static_cast<size_t>(g_frameInterpolationMode)];
                        aurora_set_frame_interpolation_fps(target);
                        RuntimeConfigFile::SetFrameInterpolationFps(target);
                        LimitResolutionForFrameRate();
                        if (aurora_get_display_mode() == AURORA_DISPLAY_MODE_EXCLUSIVE) aurora_set_display_mode(AURORA_DISPLAY_MODE_EXCLUSIVE);
                    }});
    // How the frames above 60 are made, only when there are any. Predict first: it is the recommended one (and the
    // default until a player picks), since interpolating costs a frame of delay.
    if (IsHighFrameRateMode()) {
        rows.push_back({"Frame interpolation", g_framePredict ? "PREDICT" : "INTERPOLATE",
                        g_framePredict ? "Recommended. Adds no delay; may glitch briefly during fast motion."
                                       : "More accurate in-between frames. Adds 1 frame of input delay.",
                        [](int) {
                            g_framePredict = !g_framePredict;
                            aurora_set_frame_interpolation_mode(g_framePredict ? AURORA_FRAME_INTERPOLATION_PREDICT
                                                                               : AURORA_FRAME_INTERPOLATION_INTERPOLATE);
                            RuntimeConfigFile::SetFrameInterpolationMode(g_framePredict ? "predict" : "interpolate");
                        }});
    }
    VSyncRow(rows);
    const bool bloom = (g_disabledPostProcessingPaths & 0x10u) == 0;
    rows.push_back({"Bloom", OnOff(bloom), "The glow around bright light. Applies from the next scene.", [bloom](int) {
                        g_disabledPostProcessingPaths = bloom ? (g_disabledPostProcessingPaths | 0x10u) : (g_disabledPostProcessingPaths & ~0x10u);
                        RuntimeGameGraphicsOptions::SetDisabledPostProcessingPaths(g_disabledPostProcessingPaths);
                        RuntimeConfigFile::SetDisabledPostProcessingPaths(g_disabledPostProcessingPaths);
                    }});
    PictureRows(rows);
    rows.push_back({"Show FPS", OnOff(g_showFps), "Frames per second in the corner.", [](int) {
                        g_showFps = !g_showFps;
                        RuntimeConfigFile::SetShowFps(g_showFps);
                    }});
    rows.push_back({"Hide first-time shader hitches", OnOff(g_skipUnreadyPipelines),
                    "Skips a draw for a frame while its shader is still being built, instead of pausing.", [](int) {
                        g_skipUnreadyPipelines = !g_skipUnreadyPipelines;
                        aurora_set_skip_unready_pipelines(g_skipUnreadyPipelines);
                        RuntimeConfigFile::SetSkipUnreadyPipelines(g_skipUnreadyPipelines);
                    }});
    // Takes effect at the next start: the renderer is chosen once.
#if defined(_WIN32)
    static const std::vector<std::pair<const char*, const char*>> kApis = {{"auto", "AUTOMATIC"}, {"d3d12", "DIRECT3D 12"}, {"vulkan", "VULKAN"}};
#else
    static const std::vector<std::pair<const char*, const char*>> kApis = {{"auto", "AUTOMATIC"}, {"vulkan", "VULKAN"}};
#endif
    const std::string api = RuntimeConfigFile::GraphicsApi("auto");
    int apiIndex = 0;
    for (size_t i = 0; i < kApis.size(); ++i) if (api == kApis[i].first) apiIndex = static_cast<int>(i);
    rows.push_back({"Graphics API", kApis[static_cast<size_t>(apiIndex)].second,
                    std::string("Now running on ") + GraphicsApiDisplayName() +
                        ". If the game shows a black screen or won't start, try the other one. Takes effect after a restart.",
                    [apiIndex](int step) {
                        RuntimeConfigFile::SetGraphicsApi(kApis[static_cast<size_t>(Cycle(apiIndex, step, static_cast<int>(kApis.size())))].first);
                        g_menu.restartNeeded = true;
                    }});
    static constexpr const char* kCompat[] = {"AUTOMATIC", "ON", "OFF"};
    const int compat = std::clamp(RuntimeConfigFile::GpuCompat(), 0, 2);
    rows.push_back({"GPU compatibility mode", kCompat[compat],
                    "Graphics driver workarounds, for a GPU that draws the game wrong; Automatic picks them by GPU. Takes effect after a restart.",
                    [compat](int step) {
                        RuntimeConfigFile::SetGpuCompat(Cycle(compat, step, 3));
                        g_menu.restartNeeded = true;
                    }});
    rows.push_back({"HD texture packs", "OPEN FOLDER", "Put a pack's RSBE01 folder (Dolphin's layout) in here, then restart.",
                    [](int step) { if (step == 0) OpenFolder(RuntimeConfigFile::ApplicationDataDirectory() / "texture_replacements"); }});
}

void VolumeRow(std::vector<MenuRow>& rows, const char* label, int& percent, void (*apply)(float), bool (*save)(float)) {
    rows.push_back({label, std::to_string(percent) + "%", "Left and right change it.", [&percent, apply, save](int step) {
                        if (step == 0) return;
                        percent = std::clamp(percent + step * 5, 0, 100);
                        apply(percent / 100.0f);
                        save(percent / 100.0f);
                    }, true, percent, [&percent, apply, save](int value, bool letGo) {
                        percent = std::clamp(value, 0, 100);
                        apply(percent / 100.0f);
                        if (letGo) save(percent / 100.0f);
                    }});
}

// Master volume and Mute only. Music, Sound effects, Voices, Menu sounds and "Quiet music while other media plays"
// came from Mario Kart Wii's recomp (MusicAttenuation): its sound buses were never found in Brawl
// (ResolveSoundPlayerArray returns 0), so those rows changed nothing (Brawl Online board: "audio settings seem bugged").
void AudioRows(std::vector<MenuRow>& rows) {
    VolumeRow(rows, "Master volume", g_audioVolumePercent, [](float v) { AudioBackend::Instance().SetMasterVolume(v); },
              RuntimeConfigFile::SetAudioVolume);
    rows.push_back({"Mute", OnOff(g_audioMuted), std::string("Also on the ") + KeyBindingName(g_muteHotkey) + " key.", [](int) {
                        g_audioMuted = !g_audioMuted;
                        AudioBackend::Instance().SetMuted(g_audioMuted);
                        RuntimeConfigFile::SetAudioMuted(g_audioMuted);
                    }});
}

void WiiRemotesRow(std::vector<MenuRow>& rows) {
    rows.push_back({"Wii Remotes", OnOff(g_wiiRemotesEnabled),
                    "Wii Remotes and Wii U Pro Controllers paired over Bluetooth (press 1+2 to connect). Takes effect after a restart.",
                    [](int) {
                        g_wiiRemotesEnabled = !g_wiiRemotesEnabled;
                        RuntimeConfigFile::SetWiiRemotesEnabled(g_wiiRemotesEnabled);
                        g_menu.restartNeeded = true;
                    }});
}

void ControlsRows(std::vector<MenuRow>& rows) {
    const uint32_t port = static_cast<uint32_t>(g_controllerPort);
    const char* name = PADGetName(port);
    std::string playerHint = std::string("Controller on this port: ") + (name != nullptr ? name : "none") + ". Every pad is set up automatically.";
#if MKW_BRAWL_ONLINE_CORE
    // Brawl Online, a port Dolphin's Controllers settings play: their device ("DInput/0/Keyboard Mouse").
    const bool dolphinPort = BrawlOnlineCore::DolphinControlsActive() && BrawlOnlineCore::DolphinControlsSiDevice(static_cast<int>(port)) == 6;
    if (dolphinPort) playerHint = "Controller on this port: " + BrawlOnlineCore::DolphinControlsDevice(static_cast<int>(port)) + ".";
#endif
    rows.push_back({"Player", "PLAYER " + std::to_string(port + 1), playerHint,
                    [](int step) { g_controllerPort = Cycle(g_controllerPort, step, PAD_MAX_CONTROLLERS); }});
    rows.push_back({"Vibration", OnOff(g_rumbleEnabled), "Rumble on every controller.", [](int) {
                        g_rumbleEnabled = !g_rumbleEnabled;
                        PAD_HLE_SetRumbleEnabled(g_rumbleEnabled);
                        RuntimeConfigFile::SetRumbleEnabled(g_rumbleEnabled);
                        if (!g_rumbleEnabled) {
                            constexpr std::array<uint32_t, PAD_MAX_CONTROLLERS> stopAll{PAD_MOTOR_STOP_HARD, PAD_MOTOR_STOP_HARD,
                                                                                         PAD_MOTOR_STOP_HARD, PAD_MOTOR_STOP_HARD};
                            PADControlAllMotors(stopAll.data());
                        }
                    }});
#if MKW_BRAWL_ONLINE_CORE
    // Brawl Online: the controls set up in Dolphin, and Dolphin's own window to change them over the game.
    if (BrawlOnlineCore::DolphinControllersAvailable()) {
        const bool showing = BrawlOnlineCore::DolphinControllersShowing();
        rows.push_back({"Controllers", "OPEN", "",
                        [](int step) {
                            if (step == 0) BrawlOnlineCore::OpenDolphinControllers();
                        },
                        !showing});
    }
    if (dolphinPort) {
        // Dolphin's settings play this port: the runtime's own keys and buttons below would do nothing.
        WiiRemotesRow(rows);
        return;
    }
#endif
    uint32_t keyCount = 0;
    auto* keys = PADGetKeyButtonBindings(port, &keyCount);
    rows.push_back({"Keyboard and mouse", OnOff(keys != nullptr), "Use the keyboard as this player's controller instead of a pad.",
                    [port, keys](int) {
                        PADSetKeyboardActive(port, keys == nullptr);
                        uint32_t count = 0;
                        auto* now = PADGetKeyButtonBindings(port, &count);
                        if (now != nullptr && std::all_of(now, now + count, [](const auto& b) { return b.scancode == PAD_KEY_INVALID; })) {
                            const std::array<int, PAD_BUTTON_COUNT> preset = {
                                PAD_KEY_MOUSE_LEFT, SDL_SCANCODE_SPACE, SDL_SCANCODE_E, SDL_SCANCODE_Q, SDL_SCANCODE_RETURN, PAD_KEY_MOUSE_MIDDLE,
                                SDL_SCANCODE_LSHIFT, PAD_KEY_MOUSE_RIGHT, SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT};
                            for (size_t i = 0; i < preset.size(); ++i) PADSetKeyButtonBinding(port, {preset[i], kControllerButtons[i].padButton});
                            const std::array<int, PAD_AXIS_COUNT> axes = {SDL_SCANCODE_D, SDL_SCANCODE_A, SDL_SCANCODE_W, SDL_SCANCODE_S,
                                                                          SDL_SCANCODE_L, SDL_SCANCODE_J, SDL_SCANCODE_I, SDL_SCANCODE_K,
                                                                          SDL_SCANCODE_LSHIFT, PAD_KEY_MOUSE_RIGHT};
                            uint32_t axisCount = 0;
                            auto* mappings = PADGetKeyAxisBindings(port, &axisCount);
                            for (uint32_t i = 0; i < axisCount; ++i) PADSetKeyAxisBinding(port, {axes[i], mappings[i].padAxis, 1});
                        }
                        PADSerializeMappings();
                    }});
    if (keys != nullptr) {
        for (uint32_t i = 0; i < keyCount; ++i) {
            const auto binding = keys[i];
            rows.push_back({std::string("Key: ") + PADGetButtonName(binding.padButton), KeyBindingName(binding.scancode),
                            "A or Enter, then press a key or click a mouse button. Backspace clears it.", [binding](int step) {
                                if (step == 0) BeginRebind(RebindKind::KeyboardButton, binding.padButton, PADGetButtonName(binding.padButton));
                            }});
        }
        uint32_t axisCount = 0;
        auto* axes = PADGetKeyAxisBindings(port, &axisCount);
        for (uint32_t i = 0; i < axisCount; ++i) {
            const auto binding = axes[i];
            const char* direction = PADGetAxisDirectionLabel(binding.padAxis);
            const std::string label = std::string("Key: ") + PADGetAxisName(binding.padAxis) + " " + (direction ? direction : "");
            rows.push_back({label, KeyBindingName(binding.scancode), "A or Enter, then press a key. Backspace clears it.",
                            [binding, label](int step) { if (step == 0) BeginRebind(RebindKind::KeyboardAxis, binding.padAxis, label.c_str()); }});
        }
        return;
    }
    // The controller's buttons: which of its buttons is each GameCube button. A layout swaps the face buttons at once.
    const bool padHere = PADGetIndexForPort(port) >= 0;
    uint32_t count = 0;
    const PADButtonMapping* mappings = PADGetButtonMappings(port, &count);
    const auto nativeFor = [&](uint16_t padButton) {
        for (uint32_t i = 0; mappings != nullptr && i < count; ++i)
            if (mappings[i].padButton == padButton) return mappings[i].nativeButton;
        return static_cast<uint32_t>(PAD_NATIVE_BUTTON_INVALID);
    };
    const bool nintendo = nativeFor(kControllerButtons[0].padButton) == FindNativeButton("east")->nativeButton;
    rows.push_back({"Button layout", nintendo ? "NINTENDO" : "BY POSITION",
                    "By position: A is the bottom face button, as on a GameCube pad. Nintendo: A on the right, B at the bottom, "
                    "as printed on Switch and Wii U pads.",
                    [port, nintendo](int) {
                        const auto& preset = nintendo ? kPlayStationPreset : kClassicProPreset;
                        for (size_t i = 0; i < kControllerButtons.size(); ++i) {
                            if (const NativeButtonItem* native = FindNativeButton(preset[i])) {
                                PADSetButtonMapping(port, PADButtonMapping{native->nativeButton, kControllerButtons[i].padButton});
                                PADSetAltButtonMapping(port, PADButtonMapping{PAD_NATIVE_BUTTON_INVALID, kControllerButtons[i].padButton});
                                RuntimeConfigFile::SetControllerButton(i, preset[i]);
                            }
                        }
                        PADSerializeMappings();
                    }, padHere});
    for (size_t i = 0; i < kControllerButtons.size(); ++i) {
        const uint16_t padButton = kControllerButtons[i].padButton;
        const bool waiting = g_rebind.active && g_rebind.kind == RebindKind::Controller && g_rebind.target == padButton;
        std::string value = waiting ? "PRESS A BUTTON..." : NativeButtonForValue(nativeFor(padButton)).label;
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
        rows.push_back({std::string("Button: ") + kControllerButtons[i].label, value,
                        padHere ? "A or Enter, then press the controller button to use for it."
                                : "Connect a controller for this player to change its buttons.",
                        [padButton, i](int step) {
                            if (step == 0) BeginRebind(RebindKind::Controller, padButton, kControllerButtons[i].label);
                        }, padHere});
    }
    WiiRemotesRow(rows);
}

void GameplayRows(std::vector<MenuRow>& rows) {
    rows.push_back({"Fast-forward", OnOff(g_fastForward.load()), "Also F8. Off during online play.", [](int) { g_fastForward.store(!g_fastForward.load()); }});
    const int speed = RuntimeConfigFile::FastForwardSpeed();
    rows.push_back({"Fast-forward speed", std::to_string(speed) + "X", "How fast fast-forward runs the game.",
                    [speed](int step) { RuntimeConfigFile::SetFastForwardSpeed(2 + Cycle(speed - 2, step, 3)); }});
    rows.push_back({"Fast menus", OnOff(RuntimeConfigFile::FastMenus()),
                    "Screen changes, loading and info screens run 3x as fast. Menus you move a cursor in, fights and online play don't.",
                    [](int) { RuntimeConfigFile::SetFastMenus(!RuntimeConfigFile::FastMenus()); }});
    const int slot = SaveStates::Slot();
    rows.push_back({"Save state slot", "SLOT " + std::to_string(slot + 1), SaveStates::Describe(slot),
                    [slot](int step) { SaveStates::SetSlot(Cycle(slot, step, SaveStates::kSlots)); }});
    rows.push_back({"Save state", "F5", "Saves the game exactly as it is now into the slot: anywhere, mid-fight or mid-level.",
                    [](int step) { if (step == 0) { SaveStates::RequestSave(); Flash("Saving to slot " + std::to_string(SaveStates::Slot() + 1) + "..."); } },
                    SaveStates::Available()});
    rows.push_back({"Load state", "F7", "Goes back to what the slot holds.",
                    [](int step) { if (step == 0) { SaveStates::RequestLoad(); SetTopBarVisible(false); } },
                    SaveStates::Available() && SaveStates::Exists(slot)});
}

void CheatRows(std::vector<MenuRow>& rows) {
    auto& cheats = Gecko::Cheats();
    const bool available = Gecko::CheatsAvailable();
    int needValues = 0;
    // Hundreds of codes: A switches one, left / right jump a page. Codes still holding XXXXXXXX placeholders stay in
    // the file only.
    for (size_t i = 0; i < cheats.size(); ++i) {
        const auto& cheat = cheats[i];
        if (!cheat.usable) {
            ++needValues;
            continue;
        }
        std::string hint = !available ? "Cheats are off in online play."
                           : cheat.notes.empty() ? "A switches it. Some cheats change the game's code: those stay on until you restart."
                                                 : cheat.notes;
        rows.push_back({cheat.name, OnOff(cheat.enabled), hint, [i](int step) {
                            if (step != 0) {
                                g_menu.row += step * kVisibleRows;
                                g_menu.follow = true;
                                return;
                            }
                            auto& list = Gecko::Cheats();
                            if (!Gecko::SetCheatEnabled(i, !list[i].enabled)) Flash(Gecko::CheatProblem());
                        }, available});
    }
    rows.push_back({"Add cheats", "OPEN FILE",
                    (needValues > 0 ? std::to_string(needValues) + " more codes in the file need values filled in (XXXXXXXX). " : std::string()) +
                    "Gecko codes in Dolphin's format: a $Name line, then its code lines; a Dolphin RSBE01.ini can be pasted in as it is. "
                    "Restart after editing.",
                    [](int step) { if (step == 0) OpenFolder(std::filesystem::u8path(Gecko::CheatFile()).parent_path()); }});
}

std::vector<std::string> SdBuilds() {
    std::vector<std::string> builds;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(RuntimeConfigFile::ApplicationDataDirectory() / "SD", ec)) {
        if (!entry.is_directory(ec)) continue;
        for (const auto& file : std::filesystem::directory_iterator(entry.path(), ec)) {
            std::string n = file.path().filename().string();
            std::transform(n.begin(), n.end(), n.begin(), [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
            if (n == "RSBE01.GCT") builds.push_back(RuntimeConfigFile::PathToUtf8(entry.path().filename()));
        }
    }
    std::sort(builds.begin(), builds.end());
    return builds;
}

// "Add a mod": the shallowest folder holding RSBE01.GCT inside the one picked is the mod's build; it is copied onto the
// SD card (launcher/ModInstall.cs does the same for a .zip).
void AddMod(std::filesystem::path source) {
    namespace fs = std::filesystem;
    std::error_code ec;
    std::vector<fs::path> level{source};
    fs::path build;
    for (int depth = 0; depth < 5 && build.empty() && !level.empty(); ++depth) {
        std::vector<fs::path> next;
        for (const auto& dir : level) {
            for (const auto& entry : fs::directory_iterator(dir, ec)) {
                std::string n = entry.path().filename().string();
                std::transform(n.begin(), n.end(), n.begin(), [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
                if (entry.is_regular_file(ec) && n == "RSBE01.GCT" && build.empty()) build = dir;
                if (entry.is_directory(ec)) next.push_back(entry.path());
            }
        }
        level.swap(next);
    }
    if (build.empty()) {
        g_menu.modResult = "No Wii SD card build in there (no RSBE01.GCT). Use the mod's Wii download, extracted.";
        g_menu.modCopy = -1;
        return;
    }
    const fs::path target = RuntimeConfigFile::ApplicationDataDirectory() / "SD" / build.filename();
    int copied = 0;
    for (const auto& entry : fs::recursive_directory_iterator(build, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        const fs::path dest = target / fs::relative(entry.path(), build, ec);
        fs::create_directories(dest.parent_path(), ec);
        fs::copy_file(entry.path(), dest, fs::copy_options::overwrite_existing, ec);
        g_menu.modCopy = ++copied;
    }
    const std::string name = RuntimeConfigFile::PathToUtf8(build.filename());
    RuntimeConfigFile::SetModsSdCodes(name);
    g_menu.modResult = "Added " + name + " (" + std::to_string(copied) + " files). Restart to play it.";
    g_menu.restartNeeded = true;
    g_menu.modCopy = -1;
}

void ModRows(std::vector<MenuRow>& rows) {
    const auto builds = SdBuilds();
    std::vector<std::string> choices{"", "vanilla"};
    choices.insert(choices.end(), builds.begin(), builds.end());
    const std::string chosen = RuntimeConfigFile::ModsSdCodes();
    int index = 0;
    for (size_t i = 0; i < choices.size(); ++i) if (choices[i] == chosen) index = static_cast<int>(i);
    const std::string value = index == 0 ? "FIRST ON THE CARD" : index == 1 ? "VANILLA BRAWL" : choices[static_cast<size_t>(index)];
    rows.push_back({"Build to play", value, "Which mod's codes load (REX, Project+...). Takes effect after a restart.",
                    [choices, index](int step) {
                        RuntimeConfigFile::SetModsSdCodes(choices[static_cast<size_t>(Cycle(index, step, static_cast<int>(choices.size())))]);
                        g_menu.restartNeeded = true;
                    }});
    const int copying = g_menu.modCopy.load();
    rows.push_back({"Add a mod", copying >= 0 ? "COPYING " + std::to_string(copying) : "CHOOSE FOLDER",
                    g_menu.modResult.empty() ? "Pick the folder a mod's Wii download was extracted to. It goes onto the SD card."
                                             : g_menu.modResult,
                    [](int step) {
                        if (step != 0 || g_menu.modCopy >= 0) return;
                        SDL_ShowOpenFolderDialog([](void*, const char* const* list, int) {
                            if (list == nullptr || list[0] == nullptr) return;
                            std::lock_guard<std::mutex> lock(g_menu.pendingMutex);
                            g_menu.pendingMod = list[0];
                        }, nullptr, nullptr, nullptr, false);
                    }, copying < 0});
    rows.push_back({"SD card folder", "OPEN", "The mods' files (UserData\\SD), read straight from here.",
                    [](int step) { if (step == 0) OpenFolder(RuntimeConfigFile::ApplicationDataDirectory() / "SD"); }});
    rows.push_back({"File and texture mods", "OPEN", "Files that replace the disc's (UserData\\mods): fighters, stages, music.",
                    [](int step) { if (step == 0) OpenFolder(RuntimeConfigFile::ApplicationDataDirectory() / "mods"); }});
    rows.push_back({"Code mods", "OPEN", "Recompiled code mods (UserData\\recomp_mods): new behaviour, not just files.",
                    [](int step) { if (step == 0) OpenFolder(RuntimeConfigFile::ApplicationDataDirectory() / "recomp_mods"); }});
}

void SystemRows(std::vector<MenuRow>& rows) {
    const std::string update = Updates::Available();
    rows.push_back({"Version", BRAWL_VERSION,
                    update.empty() ? "You have the latest version." : "Version " + update + " is out: Update below installs it.",
                    [](int) {}});
    if (!update.empty()) {
        rows.push_back({"Update to " + update, "INSTALL", "Downloads it and restarts the game. Your saves, settings, mods and disc stay.",
                        [](int step) { if (step == 0 && !Updates::Install()) Flash("Couldn't start the updater."); }});
    }
    rows.push_back({"Saves", "OPEN FOLDER", "Your Brawl save data (UserData\\Saves).",
                    [](int step) { if (step == 0) OpenFolder(RuntimeConfigFile::ApplicationDataDirectory() / "Saves"); }});
    rows.push_back({"Restart the game", "RESTART", "Starts Brawl again, to apply changes that need it.",
                    [](int step) { if (step == 0) NetplayStart::RestartGame(); }});
}

// Brawl Online's build plays Project+ online only, started and updated by Brawl Online's launcher: no recomp netplay
// (Online), no fast-forward, fast menus or save states (Gameplay), no cheats or mod builds (they would differ from the
// other player's game), no updater or restart of its own (System: a restart is a new process the launcher doesn't know,
// started without the core's stop). The mod support underneath stays; only its menus are left out.
bool TabShown(int tab) {
#if MKW_BRAWL_ONLINE_CORE
    return tab == kTabResume || tab == kTabGraphics || tab == kTabAudio || tab == kTabControls || tab == kTabQuit;
#else
    (void)tab;
    return true;
#endif
}

// The next tab shown, from `tab` in `step`'s direction (+1 / -1).
int StepTab(int tab, int step) {
    for (int i = 0; i < kTabCount; ++i) {
        tab = (tab + step + kTabCount) % kTabCount;
        if (TabShown(tab)) break;
    }
    return tab;
}

std::vector<MenuRow> TabRows(int tab) {
    std::vector<MenuRow> rows;
    switch (tab) {
    case kTabGraphics: GraphicsRows(rows); break;
    case kTabAudio: AudioRows(rows); break;
    case kTabControls: ControlsRows(rows); break;
#if !MKW_BRAWL_ONLINE_CORE
    case kTabGameplay: GameplayRows(rows); break;
    case kTabCheats: CheatRows(rows); break;
    case kTabMods: ModRows(rows); break;
    case kTabSystem: SystemRows(rows); break;
#endif
    default: break;
    }
#if !MKW_BRAWL_ONLINE_CORE
    if (g_menu.restartNeeded && (tab == kTabMods || tab == kTabControls || tab == kTabGraphics))
        rows.push_back({"Restart now", "RESTART", "Some changes here take effect after a restart.",
                        [](int step) { if (step == 0) NetplayStart::RestartGame(); }});
#endif
    return rows;
}

// ---- drawing ----
// Layout units: 456 to the game picture's height, y up, 0 in the middle (MenuKit's canvas). The menu keeps inside
// 1000 x 480 of them, so its slanted edges and glows stay inside any window, 4:3 to ultrawide (DrawOptionsMenu's scale).
constexpr float kRowX0 = -190, kRowX1 = 428;  // the rows' plates
constexpr float kRowTop = 144, kRowStep = 40, kRowHalf = 17;
constexpr float kValX0 = 196, kValX1 = 420, kValMid = (kValX0 + kValX1) * 0.5f;  // the value column
constexpr float kSliderX0 = 210, kSliderX1 = 356;                                // a slider's track
constexpr float kScrollBarX = 452, kScrollBarHalfWidth = 4;
constexpr float kTabX0 = -458, kTabX1 = -222;
constexpr float kListTop = kRowTop + kRowHalf + 4, kListBottom = kRowTop - (kVisibleRows - 1) * kRowStep - kRowHalf - 4;
constexpr float kHintY = -172, kFooterY = -219;

enum class RowKind { Choice, Toggle, Slider, Action };

bool IsActionValue(const std::string& value) {
    static const std::set<std::string> kActions = {"", "OPEN", "OPEN FOLDER", "OPEN FILE", "RESTART", "INSTALL",
                                                   "CHOOSE FOLDER", "F5", "F7"};
    return kActions.count(value) != 0;
}

RowKind KindOf(const MenuRow& row) {
    if (row.bar >= 0) return RowKind::Slider;
    // A key or button binding: A starts it, left and right have nothing to go through.
    if (row.action || IsActionValue(row.value) || row.label.rfind("Key: ", 0) == 0 || row.label.rfind("Button: ", 0) == 0)
        return RowKind::Action;
    if (row.value == "ON" || row.value == "OFF") return RowKind::Toggle;
    return RowKind::Choice;
}

// A slanted shape like Plate's, flat and without an edge: the value column's well.
void SlantFill(const MK::Canvas& c, float x0, float y0, float x1, float y1, float slant, uint32_t color, float alpha) {
    c.list->AddQuadFilled(c.P(x0 + slant, y1), c.P(x1 + slant, y1), c.P(x1 - slant, y0), c.P(x0 - slant, y0), MK::Col(color, alpha));
}

void ValueText(const MK::Canvas& c, const std::string& text, float x, float y, float size, uint32_t top, uint32_t bottom,
               int originH, float maxWidth, float alpha = 1.0f) {
    std::string value = text;  // Brawl's title font has no parentheses
    if (!MK::HaveVectorFonts()) value.erase(std::remove_if(value.begin(), value.end(), [](char ch) { return ch == '(' || ch == ')'; }), value.end());
    if (MK::HaveVectorFonts()) MK::VText(c, MK::Fonts().display, value, x, y, size, top, bottom, originH, maxWidth, false, alpha);
    else MK::Text(c, *MK::TitleFont(), value, x, y, size, top, bottom, originH, maxWidth, true);
}

// A slider's position (0..100) under the mouse.
int SliderValueAt(const MK::Canvas& c, float mouseX) {
    const float x = (mouseX - c.view.centerX) / c.view.scale;
    return static_cast<int>(std::lround(std::clamp((x - kSliderX0) / (kSliderX1 - kSliderX0), 0.0f, 1.0f) * 100.0f));
}

void SliderDraw(const MK::Canvas& c, const MenuRow& row, float y, bool focused, float alpha) {
    const float r = c.view.scale * 3.5f;
    const float knob = kSliderX0 + (kSliderX1 - kSliderX0) * std::clamp(row.bar, 0, 100) / 100.0f;
    c.list->AddRectFilled(c.P(kSliderX0, y + 3.5f), c.P(kSliderX1, y - 3.5f), MK::Col(MK::kInk, alpha), r);
    const float from = row.centered ? (kSliderX0 + kSliderX1) * 0.5f : kSliderX0;
    if (std::fabs(knob - from) > 0.5f) {
        const float a = std::min(from, knob), b = std::max(from, knob);
        c.list->AddRectFilled(c.P(a, y + 3.5f), c.P(b, y - 3.5f), MK::Col(focused ? MK::kOrange : 0xC86A20FFu, alpha), r);
    }
    if (row.centered) c.list->AddRectFilled(c.P(from - 0.6f, y + 6), c.P(from + 0.6f, y - 6), MK::Col(MK::kDim, alpha * 0.6f));
    c.list->AddRect(c.P(kSliderX0, y + 3.5f), c.P(kSliderX1, y - 3.5f), MK::Col(0x3A4160FFu, alpha), r, 0, std::max(1.0f, c.view.scale * 0.6f));
    const float kr = (focused ? 8.5f : 7.0f) * c.view.scale;
    const ImVec2 k = c.P(knob, y);
    c.list->AddCircleFilled(ImVec2(k.x, k.y + c.view.scale), kr + c.view.scale, MK::Col(MK::kInk, alpha * 0.6f), 24);
    c.list->AddCircleFilled(k, kr, MK::Col(MK::kSnow, alpha), 24);
    c.list->AddCircle(k, kr, MK::Col(focused ? MK::kOrange : 0x8A8FA8FFu, alpha), 24, std::max(1.0f, c.view.scale * 1.6f));
    ValueText(c, row.value, kValX1 - 4, y, 17, MK::kSnow, 0xFFE7B0FFu, 2, kValX1 - kSliderX1 - 18, alpha);
}

void ToggleDraw(const MK::Canvas& c, bool on, float y, bool focused, float alpha) {
    const float w = 66, h = 24, x0 = kValMid - w * 0.5f, x1 = kValMid + w * 0.5f;
    const float r = h * 0.5f * c.view.scale;
    c.list->AddRectFilled(c.P(x0, y + h * 0.5f), c.P(x1, y - h * 0.5f), MK::Col(on ? MK::kOrange : MK::kInk, alpha), r);
    c.list->AddRect(c.P(x0, y + h * 0.5f), c.P(x1, y - h * 0.5f), MK::Col(on ? 0xFFC86AFFu : (focused ? 0x6A7090FFu : 0x3A4160FFu), alpha),
                    r, 0, std::max(1.0f, c.view.scale * 1.2f));
    const float kx = on ? x1 - h * 0.5f : x0 + h * 0.5f;
    c.list->AddCircleFilled(c.P(kx, y), (h * 0.5f - 3.5f) * c.view.scale, MK::Col(on ? MK::kSnow : 0x8A8FA8FFu, alpha), 24);
    ValueText(c, on ? "ON" : "OFF", on ? x0 + 21 : x1 - 23, y, 14, on ? 0x2A1A00FFu : MK::kDim, on ? 0x4A3000FFu : MK::kDim, 1, 30, alpha);
}

void ChoiceDraw(const MK::Canvas& c, const MenuRow& row, float y, bool focused, bool hovered, float alpha) {
    SlantFill(c, kValX0, y - 12, kValX1, y + 12, 4.0f, 0x0F1220FFu, alpha * 0.85f);
    ValueText(c, row.value, kValMid, y, 18, MK::kSnow, 0xFFE7B0FFu, 1, kValX1 - kValX0 - 64, alpha);
    if (!row.enabled || !row.change) return;
    // The arrows say left and right go through it: orange on the focused row, faint on the rest.
    const uint32_t arrow = focused ? MK::kOrange : hovered ? 0x9EA3BCFFu : 0x4E5470FFu;
    MK::Arrow(c, kValX0 + 16, y, 11, 2, arrow);
    MK::Arrow(c, kValX1 - 16, y, 11, 3, arrow);
}

void ActionDraw(const MK::Canvas& c, const MenuRow& row, float y, bool focused, float alpha) {
    const std::string text = row.value.empty() ? std::string() : row.value;
    if (text.empty()) return;
    const float w = std::min(kValX1 - kValX0 - 8,
                             (MK::HaveVectorFonts() ? MK::VTextWidth(c, MK::Fonts().display, text, 15) : 9.0f * text.size()) + 34);
    const float x0 = kValMid - w * 0.5f, x1 = kValMid + w * 0.5f;
    MK::Plate(c, x0, y - 12, x1, y + 12, focused ? 0x4A5A90FFu : 0x323B5EFFu, 5.0f, alpha);
    if (focused) MK::PlateOutline(c, x0, y - 12, x1, y + 12, 5.0f, 2.0f, MK::kOrange, alpha, 1.2f);
    ValueText(c, text, kValMid, y, 15, MK::kSnow, 0xFFE7B0FFu, 1, w - 18, alpha);
}

void MenuRowDraw(const MK::Canvas& c, const MenuRow& row, float y, bool focused, bool hovered) {
    const float alpha = row.enabled ? 1.0f : 0.5f;
    MK::Plate(c, kRowX0, y - kRowHalf, kRowX1, y + kRowHalf, focused ? 0x2B3350FFu : 0x1C2133FFu, 7.0f, row.enabled ? 1.0f : 0.6f);
    if (focused) {
        // A steady orange edge in the plate's own slant (it was a blinking rectangle around a slanted plate), and a bar
        // at its left end.
        const float pulse = 0.8f + 0.2f * std::sin(c.t * 4.0f);
        MK::PlateOutline(c, kRowX0, y - kRowHalf, kRowX1, y + kRowHalf, 7.0f, 2.2f, MK::kOrange, pulse, 1.4f);
        c.list->AddQuadFilled(c.P(kRowX0 + 7 + 2, y + kRowHalf - 3), c.P(kRowX0 + 7 + 7, y + kRowHalf - 3),
                              c.P(kRowX0 - 7 + 7 + 3.5f, y - kRowHalf + 3), c.P(kRowX0 - 7 + 2 + 3.5f, y - kRowHalf + 3), MK::Col(MK::kOrange));
    }
    MK::Body(c, row.label, kRowX0 + 22, y, 16, row.enabled ? (focused ? 0xFFFFFFFFu : MK::kSnow) : MK::kDim, 0, kValX0 - kRowX0 - 40);
    switch (KindOf(row)) {
    case RowKind::Slider: SliderDraw(c, row, y, focused, alpha); break;
    case RowKind::Toggle: ToggleDraw(c, row.value == "ON", y, focused, alpha); break;
    case RowKind::Action: ActionDraw(c, row, y, focused, alpha); break;
    case RowKind::Choice: ChoiceDraw(c, row, y, focused, hovered, alpha); break;
    }
}

// The list's scroll bar: a track the list's height with a thumb as tall as the share of rows shown. The thumb can be
// dragged, and a click on the track above or below it pages the list. Only drawn when the rows don't all fit.
void ScrollBarDraw(const MK::Canvas& c, float top, float bottom, int count, float maxScroll, bool capturing) {
    if (maxScroll <= 0.0f) {
        g_menu.dragging = false;
        return;
    }
    const ImGuiIO& io = ImGui::GetIO();
    const float trackH = top - bottom;
    const float thumbH = std::max(28.0f, trackH * kVisibleRows / static_cast<float>(count));
    const float thumbTop = top - (trackH - thumbH) * g_menu.scroll / maxScroll;
    const float x0 = kScrollBarX - kScrollBarHalfWidth, x1 = kScrollBarX + kScrollBarHalfWidth;
    const float mouseY = (c.view.centerY - io.MousePos.y) / c.view.scale;  // layout units, y up
    // A wider grab area than the bar itself, so it is easy to hit.
    const bool overTrack = !capturing && ImGui::IsMouseHoveringRect(c.P(x0 - 8, top), c.P(x1 + 8, bottom), false);
    const bool overThumb = overTrack && mouseY <= thumbTop && mouseY >= thumbTop - thumbH;
    if (overTrack && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (overThumb) {
            g_menu.dragging = true;
            g_menu.dragOffset = thumbTop - mouseY;
        } else {
            g_menu.scrollTarget = std::clamp(g_menu.scrollTarget + (mouseY > thumbTop ? -1.0f : 1.0f) * kVisibleRows, 0.0f, maxScroll);
        }
    }
    if (g_menu.dragging) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) || capturing) {
            g_menu.dragging = false;
        } else if (trackH > thumbH) {
            const float t = std::clamp((top - (mouseY + g_menu.dragOffset)) / (trackH - thumbH), 0.0f, 1.0f);
            g_menu.scroll = g_menu.scrollTarget = t * maxScroll;  // the list follows the hand without easing
        }
    }
    const float r = kScrollBarHalfWidth * c.view.scale;
    c.list->AddRectFilled(c.P(x0, top), c.P(x1, bottom), MK::Col(0x232A42FFu), r);
    const bool lit = g_menu.dragging || overThumb;
    c.list->AddRectFilled(c.P(x0, thumbTop), c.P(x1, thumbTop - thumbH), MK::Col(lit ? 0xFFC86AFFu : MK::kOrange), r);
}

// `text` broken at spaces into lines no wider than maxPx pixels at `px`.
std::vector<std::string> WrapWords(ImFont* font, float px, const std::string& text, float maxPx) {
    std::vector<std::string> lines;
    std::string line;
    for (size_t i = 0; i <= text.size();) {
        size_t j = text.find(' ', i);
        if (j == std::string::npos) j = text.size();
        const std::string word = text.substr(i, j - i);
        const std::string trial = line.empty() ? word : line + " " + word;
        if (!line.empty() && font->CalcTextSizeA(px, FLT_MAX, 0.0f, trial.c_str()).x > maxPx) {
            lines.push_back(line);
            line = word;
        } else {
            line = trial;
        }
        i = j + 1;
    }
    if (!line.empty()) lines.push_back(line);
    return lines;
}

// The focused row's explanation under the list, in two lines at most (smaller when it is long: a cheat's notes).
void HintDraw(const MK::Canvas& c, const std::string& text, uint32_t color) {
    if (text.empty()) return;
    const float x0 = kRowX0 + 6, width = kRowX1 - kRowX0 - 12;
    if (MK::HaveVectorFonts()) {
        ImFont* font = MK::Fonts().body;
        float size = 13.5f;
        std::vector<std::string> lines;
        for (; size >= 10.0f; size -= 0.5f) {
            lines = WrapWords(font, std::max(6.0f, std::round(size * c.view.scale)), text, width * c.view.scale);
            if (lines.size() <= 2) break;
        }
        for (size_t i = 0; i < std::min<size_t>(lines.size(), 2); ++i)
            MK::VText(c, font, lines[i], x0, kHintY - (size + 4.0f) * i, size, color, color, 0);
        return;
    }
    const auto lines = GameLayout::WrapText(*MK::MenuFont(), GameLayout::Utf16(text), 15, width);
    for (size_t i = 0; i < std::min<size_t>(lines.size(), 2); ++i)
        GameLayout::DrawText(c.list, c.view, *MK::MenuFont(), lines[i], x0, kHintY - 17.0f * i, 15, color, color, 1.0f, 0, 1);
}

// A controller button or a key, as the footer and the header show them: `kind` 0 a round face button (A, B), 1 a
// long one (START, L, R), 2 a key. Returns its width.
float ButtonGlyph(const MK::Canvas& c, int kind, const std::string& name, float x, float y, uint32_t color, bool draw = true) {
    const float h = 19;
    float w = h;
    if (kind != 0) w = (MK::HaveVectorFonts() ? MK::VTextWidth(c, MK::Fonts().display, name, 12) : 8.0f * name.size()) + 14;
    w = std::max(w, h);
    if (!draw) return w;
    const ImVec2 lo = c.P(x, y + h * 0.5f), hi = c.P(x + w, y - h * 0.5f);
    if (kind == 0) {
        const ImVec2 mid = c.P(x + w * 0.5f, y);
        c.list->AddCircleFilled(mid, h * 0.5f * c.view.scale, MK::Col(color), 24);
        c.list->AddCircle(mid, h * 0.5f * c.view.scale, MK::Col(MK::kInk, 0.8f), 24, std::max(1.0f, c.view.scale));
    } else if (kind == 1) {
        c.list->AddRectFilled(lo, hi, MK::Col(color), h * 0.5f * c.view.scale);
    } else {
        c.list->AddRectFilled(lo, hi, MK::Col(0x2A3150FFu), 3.5f * c.view.scale);
        c.list->AddRect(lo, hi, MK::Col(0x8A8FA8FFu), 3.5f * c.view.scale, 0, std::max(1.0f, c.view.scale));
    }
    if (name == "<>") {
        MK::Arrow(c, x + w * 0.5f - 4.5f, y, 8, 2, MK::kSnow);
        MK::Arrow(c, x + w * 0.5f + 4.5f, y, 8, 3, MK::kSnow);
    } else {
        ValueText(c, name, x + w * 0.5f, y, kind == 0 ? 13 : 12, kind == 0 ? 0xFFFFFFFFu : MK::kSnow, kind == 0 ? 0xFFFFFFFFu : MK::kSnow, 1, w - 4);
    }
    return w;
}

// The footer: what each button does, as the controller or the keyboard has it (whichever was used last).
void FooterDraw(const MK::Canvas& c) {
    MK::Plate(c, -462, kFooterY - 15, 462, kFooterY + 15, 0x10131FFFu, 8.0f);
    struct Prompt {
        int kind;
        const char* name;
        uint32_t color;
        const char* word;
    };
    const bool pad = MK::LastInputController();
    const Prompt controller[] = {{0, "A", 0x1FA88AFFu, "choose"}, {1, "<>", 0x4A5272FFu, "change"},
                                 {0, "B", 0xD8392BFFu, "back"}, {1, "START", 0x6A7090FFu, "close"}};
    const Prompt keyboard[] = {{2, "ENTER", 0, "choose"}, {2, "<>", 0, "change"}, {2, "ESC", 0, "back"}, {2, "F10", 0, "close"}};
    const Prompt* list = pad ? controller : keyboard;
    constexpr float kWordSize = 15, kGap = 7, kSpace = 30;
    float total = 0;
    for (int i = 0; i < 4; ++i) {
        total += ButtonGlyph(c, list[i].kind, list[i].name, 0, 0, 0, false) + kGap;
        total += (MK::HaveVectorFonts() ? MK::VTextWidth(c, MK::Fonts().label, list[i].word, kWordSize * 1.1f) : 8.0f * std::strlen(list[i].word));
        if (i < 3) total += kSpace;
    }
    float x = -total * 0.5f;
    for (int i = 0; i < 4; ++i) {
        x += ButtonGlyph(c, list[i].kind, list[i].name, x, kFooterY, list[i].color) + kGap;
        MK::Body(c, list[i].word, x, kFooterY + 1.5f, kWordSize, MK::kDim, 0);  // lower case: centred on its x-height
        x += (MK::HaveVectorFonts() ? MK::VTextWidth(c, MK::Fonts().label, list[i].word, kWordSize * 1.1f) : 8.0f * std::strlen(list[i].word)) + kSpace;
    }
}

// The next tab with rows (Graphics, Audio, Controls, ...), for a controller's L and R.
int StepRowsTab(int tab, int step) {
    for (int i = 0; i < kTabCount; ++i) {
        tab = StepTab(tab, step);
        if (tab != kTabResume && tab != kTabOnline && tab != kTabQuit) break;
    }
    return tab;
}

void DrawOptionsMenu() {
    if (!g_topBarVisible) {
        g_menu.inRows = false;
        g_menu.quitArmed = false;
        g_menu.escapeHeld = false;
        g_menu.dragging = false;
        g_menu.sliderRow = -1;
        // Controllers are still read while the menu is closed, so a button already down when it opens is not a press.
        if (!OnlineMenu::IsOpen()) (void)MK::ReadNav();
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_menu.pendingMutex);
        if (!g_menu.pendingMod.empty()) {
            g_menu.modCopy = 0;
            g_menu.modResult.clear();
            std::thread(AddMod, std::filesystem::u8path(g_menu.pendingMod)).detach();
            g_menu.pendingMod.clear();
        }
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    if (ImGui::Begin("##options", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                                ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollWithMouse)) {
        MK::Canvas c{ImGui::GetWindowDrawList(), GameLayout::GameView(), static_cast<float>(ImGui::GetTime())};
        c.view.centerX = std::round(vp->Pos.x + vp->Size.x * 0.5f);
        c.view.centerY = std::round(vp->Pos.y + vp->Size.y * 0.5f);
        // The whole menu, slants and glows included, inside the window (at 16:9 its plates ran off both sides).
        c.view.scale = std::min(vp->Size.y / 480.0f, vp->Size.x / 1000.0f);
        ImGui::SetWindowFocus();
        // The game stays visible behind a dark veil, as in its own pause menu.
        c.list->AddRectFilledMultiColor(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y), IM_COL32(10, 12, 26, 228),
                                        IM_COL32(10, 12, 26, 228), IM_COL32(34, 12, 8, 238), IM_COL32(34, 12, 8, 238));
        const bool capturing = g_rebind.active;
        MK::Nav nav = capturing ? MK::Nav{} : MK::ReadNav();
        // Escape opens the menu as it goes down (HandleEvents): that press, and its repeats, are not also Back. ImGui
        // may take the press a frame or two later (its input queue), so it counts as done once ImGui saw it go down
        // and up again (or never saw it within half a second).
        if (g_menu.escapeHeld) {
            const bool down = ImGui::IsKeyDown(ImGuiKey_Escape);
            if (down) g_menu.escapeSeenDown = true;
            if ((g_menu.escapeSeenDown && !down) || (!g_menu.escapeSeenDown && ImGui::GetTime() - g_menu.escapeAt > 0.5)) {
                g_menu.escapeHeld = false;
            } else if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                nav.back = false;
            }
        }
        // START held to open the menu (PollMenuButton) is not also its first press in it.
        if (g_menu.startHeld) {
            if (!nav.startHeld) g_menu.startHeld = false;
            nav.start = false;
        }
        // START closes the menu, as it leaves a game's pause; A chooses.
        const bool close = nav.start;
        nav.confirm = nav.confirm && !nav.start && !nav.startHeld;
        if (nav.up || nav.down || nav.left || nav.right || nav.confirm || nav.back || nav.prevTab || nav.nextTab) g_menu.mouse = false;

        // Banner: OPTIONS, the tab, and (a controller last used) the L and R that change it.
        if (!TabShown(g_menu.tab)) g_menu.tab = kTabGraphics;
        MK::Plate(c, -462, 178, 210, 224, 0x14182AFFu, 14.0f);
        c.list->AddRectFilledMultiColor(c.P(-462, 176.5f), c.P(330, 173), MK::Col(MK::kOrange), MK::Col(MK::kOrange, 0.0f),
                                        MK::Col(MK::kOrange, 0.0f), MK::Col(MK::kOrange));
        MK::Text(c, *MK::TitleFont(), "OPTIONS", -442, 201, 44, 0xFFF3B0FFu, MK::kOrange, 0, 0, true);
        const float titleW = MK::HaveVectorFonts() ? MK::VTextWidth(c, MK::Fonts().display, "OPTIONS", 44 * 0.94f) : 240.0f;
        float tx = -442 + titleW + 26;
        const bool rowsTab = g_menu.tab != kTabResume && g_menu.tab != kTabOnline && g_menu.tab != kTabQuit;
        const bool padPrompts = MK::LastInputController() && rowsTab;
        if (padPrompts) tx += ButtonGlyph(c, 1, "L", tx, 199, 0x4A5272FFu) + 10;
        const float nameW = MK::HaveVectorFonts() ? std::min(200.0f, MK::VTextWidth(c, MK::Fonts().display, kTabNames[g_menu.tab], 24 * 0.94f)) : 200.0f;
        MK::Text(c, *MK::TitleFont(), kTabNames[g_menu.tab], tx, 198, 24, MK::kSnow, 0xFFC86AFFu, 0, 200, true);
        if (padPrompts) ButtonGlyph(c, 1, "R", tx + nameW + 10, 199, 0x4A5272FFu);
        MK::SmashBall(c, 404, 200, 52, c.t * 0.35f);

        // Tabs.
        int shownTabs = 0;
        for (int i = 0; i < kTabCount; ++i) shownTabs += TabShown(i) ? 1 : 0;
        const float tabStep = std::min(kRowStep, (kRowTop - (kHintY - 14)) / std::max(1, shownTabs - 1));
        const int tabFocus = g_menu.inRows ? -1 : g_menu.tab;
        for (int i = 0, shown = 0; i < kTabCount; ++i) {
            if (!TabShown(i)) continue;
            const float y = kRowTop - shown++ * tabStep;
            const float half = std::min(16.0f, tabStep * 0.5f - 3.0f);
            const uint32_t color = i == kTabQuit ? MK::kPlayer[0] : i == g_menu.tab ? 0x3A4670FFu : 0x232A42FFu;
            const std::string label = i == kTabQuit && g_menu.quitArmed ? "QUIT? PRESS AGAIN" : kTabNames[i];
            int focus = tabFocus;
            if (MK::Button(c, i, focus, false, kTabX0, y - half, kTabX1, y + half, label, color, true, 21.0f)) {
                g_menu.tab = i;
                g_menu.inRows = i != kTabResume && i != kTabOnline && i != kTabQuit;
                g_menu.row = 0;
                g_menu.scroll = g_menu.scrollTarget = 0;
                if (i == kTabResume) SetTopBarVisible(false);
                if (i == kTabOnline) { OnlineMenu::SetOpen(true); SetTopBarVisible(false); }
                if (i == kTabQuit) { if (g_menu.quitArmed) ExitForAuroraWindowClose(); g_menu.quitArmed = true; }
            }
            if (focus != tabFocus && focus == i && !g_menu.inRows) g_menu.tab = i;  // hovered
            if (i == g_menu.tab && g_menu.inRows) MK::Arrow(c, kTabX1 + 14, y, 13, 3, MK::kOrange);
        }

        // Rows of the chosen tab.
        std::vector<MenuRow> rows = TabRows(g_menu.tab);
        const int count = static_cast<int>(rows.size());
        const ImGuiIO& io = ImGui::GetIO();
        g_menu.row = std::clamp(g_menu.row, 0, std::max(0, count - 1));
        if (g_menu.scrollTab != g_menu.tab) {
            g_menu.scrollTab = g_menu.tab;
            g_menu.scroll = g_menu.scrollTarget = 0;
            g_menu.sliderRow = -1;
        }
        if (io.MouseDelta.x != 0 || io.MouseDelta.y != 0) g_menu.mouse = true;
        if (io.MouseWheel != 0 && count > 0 && !capturing) {
            g_menu.scrollTarget -= io.MouseWheel;
            g_menu.mouse = true;
        }
        if (g_menu.follow) {
            g_menu.follow = false;
            g_menu.scrollTarget = std::clamp(g_menu.scrollTarget, static_cast<float>(g_menu.row - kVisibleRows + 1),
                                             static_cast<float>(g_menu.row));
        }
        const float maxScroll = static_cast<float>(std::max(0, count - kVisibleRows));
        g_menu.scrollTarget = std::clamp(g_menu.scrollTarget, 0.0f, maxScroll);
        g_menu.scroll += (g_menu.scrollTarget - g_menu.scroll) * std::min(1.0f, io.DeltaTime * 18.0f);
        if (std::fabs(g_menu.scrollTarget - g_menu.scroll) < 0.002f) g_menu.scroll = g_menu.scrollTarget;
        g_menu.scroll = std::clamp(g_menu.scroll, 0.0f, maxScroll);
        std::string hint;
        if (count == 0) {
            const char* text = g_menu.tab == kTabResume ? "Back to the game." : g_menu.tab == kTabOnline ? "Play with friends or anyone online."
                                                                                                         : "Close Brawl Recompiled. Your settings are saved.";
            MK::Body(c, text, (kRowX0 + kRowX1) * 0.5f, 60, 20, MK::kDim, 1, kRowX1 - kRowX0);
        }

        // A slider held with the mouse follows it, and is saved when let go.
        if (g_menu.sliderRow >= 0) {
            const bool alive = g_menu.sliderRow < count && rows[static_cast<size_t>(g_menu.sliderRow)].setBar && !capturing;
            if (alive) {
                MenuRow& row = rows[static_cast<size_t>(g_menu.sliderRow)];
                const int value = SliderValueAt(c, io.MousePos.x);
                const bool held = ImGui::IsMouseDown(ImGuiMouseButton_Left);
                if (value != row.bar || !held) row.setBar(value, !held);
                if (!held) g_menu.sliderRow = -1;
                rows = TabRows(g_menu.tab);  // drawn with the new value this frame
            } else {
                g_menu.sliderRow = -1;
            }
        }

        // Rows part-way in or out are cut at the list's edges (the focus outline included).
        const ImVec2 clipLo = c.P(kRowX0 - 30, kListTop), clipHi = c.P(kScrollBarX - 12, kListBottom);
        ScrollBarDraw(c, kListTop - 4, kListBottom + 4, count, maxScroll, capturing);
        // While the scroll bar's thumb or a slider is dragged, the rows the mouse passes over are not chosen or clicked.
        const bool mouseInList = !g_menu.dragging && g_menu.sliderRow < 0 && ImGui::IsMouseHoveringRect(clipLo, clipHi, false);
        c.list->PushClipRect(clipLo, clipHi, true);
        const int firstShown = static_cast<int>(std::floor(g_menu.scroll));
        const float mouseX = (io.MousePos.x - c.view.centerX) / c.view.scale;
        for (int i = firstShown; i < std::min(count, firstShown + kVisibleRows + 1); ++i) {
            const float y = kRowTop - (i - g_menu.scroll) * kRowStep;
            const ImVec2 lo = c.P(kRowX0 - 7, y + kRowHalf), hi = c.P(kRowX1 + 7, y - kRowHalf);
            const bool hovered = mouseInList && ImGui::IsMouseHoveringRect(lo, hi);
            if (hovered && g_menu.mouse && !capturing) {
                g_menu.inRows = true;
                g_menu.row = i;
            }
            const bool focused = g_menu.inRows && g_menu.row == i;
            MenuRow& row = rows[static_cast<size_t>(i)];
            MenuRowDraw(c, row, y, focused || (g_menu.sliderRow == i), hovered);
            if (!hovered || capturing || !ImGui::IsMouseClicked(ImGuiMouseButton_Left) || !row.enabled) continue;
            // A click: a slider takes the value under the mouse (and follows it while held); a choice's arrows step
            // back and forward through it, the rest of its row forward; a switch or a button anywhere on its row.
            switch (KindOf(row)) {
            case RowKind::Slider:
                if (mouseX >= kSliderX0 - 14 && mouseX <= kSliderX1 + 14) {
                    if (row.setBar) {
                        g_menu.sliderRow = i;
                        row.setBar(SliderValueAt(c, io.MousePos.x), false);
                    } else if (row.change) {
                        row.change(SliderValueAt(c, io.MousePos.x) > row.bar ? 1 : -1);
                    }
                }
                break;
            case RowKind::Choice:
                if (row.change) row.change(mouseX >= kValX0 && mouseX < kValX0 + 34 ? -1 : mouseX > kValX1 - 34 && mouseX <= kValX1 + 8 ? 1 : 0);
                break;
            default:
                if (row.change) row.change(0);
                break;
            }
        }
        c.list->PopClipRect();
        if (g_menu.inRows && count > 0) hint = rows[static_cast<size_t>(g_menu.row)].hint;

        // Navigation.
        if (close) {
            SetTopBarVisible(false);
        } else if (nav.prevTab || nav.nextTab) {
            const int tab = StepRowsTab(g_menu.tab, nav.nextTab ? 1 : -1);
            if (tab != g_menu.tab) {
                g_menu.tab = tab;
                g_menu.row = 0;
                g_menu.scroll = g_menu.scrollTarget = 0;
                g_menu.quitArmed = false;
            }
        } else if (!g_menu.inRows) {
            if (nav.up) g_menu.tab = StepTab(g_menu.tab, -1), g_menu.quitArmed = false;
            if (nav.down) g_menu.tab = StepTab(g_menu.tab, 1), g_menu.quitArmed = false;
            if (nav.confirm || nav.right) {
                if (g_menu.tab == kTabResume) SetTopBarVisible(false);
                else if (g_menu.tab == kTabOnline) { OnlineMenu::SetOpen(true); SetTopBarVisible(false); }
                else if (g_menu.tab == kTabQuit) { if (nav.confirm && g_menu.quitArmed) ExitForAuroraWindowClose(); g_menu.quitArmed = nav.confirm; }
                else if (count > 0) g_menu.inRows = true, g_menu.row = 0, g_menu.scroll = g_menu.scrollTarget = 0;
            }
            if (nav.back) SetTopBarVisible(false);
        } else if (count > 0) {
            MenuRow& row = rows[static_cast<size_t>(g_menu.row)];
            const RowKind kind = KindOf(row);
            if (nav.up) g_menu.row = (g_menu.row + count - 1) % count, g_menu.follow = true;
            if (nav.down) g_menu.row = (g_menu.row + 1) % count, g_menu.follow = true;
            if (row.enabled && row.change) {
                // A on a slider does nothing (it used to raise it a step).
                if (nav.confirm && kind != RowKind::Slider) row.change(0);
                if (nav.right && kind != RowKind::Action) row.change(1);
                if (nav.left) {
                    // Left on a button (nothing to go left through) leads back to the tabs instead.
                    if (kind == RowKind::Action) g_menu.inRows = false;
                    else row.change(-1);
                }
            } else if (nav.left) {
                g_menu.inRows = false;
            }
            if (nav.back) g_menu.inRows = false;
        } else if (nav.back || nav.left) {
            g_menu.inRows = false;
        }

        // Under the rows: the focused row's explanation (or a message for a few seconds), then the buttons.
        const bool flashing = ImGui::GetTime() < g_menu.flashUntil && !g_menu.flash.empty();
        if (g_rebind.active) {
            hint = g_rebind.kind == RebindKind::Controller ? "Press the controller button for " + g_rebind.label + "."
                                                           : "Press a key or click a mouse button for " + g_rebind.label + ". Backspace clears it.";
        }
        HintDraw(c, flashing ? g_menu.flash : hint, flashing ? MK::kGold : MK::kDim);
        FooterDraw(c);
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    if (g_rebind.active) PollRebind(false);
}

// Home (guide) on any controller, or Back and Start held together, opens and closes the menu. A GameCube controller has
// neither: START held for a second opens it, outside a fight (the game takes no input while the menu is open, and a
// fight goes on).
void PollMenuButton() {
    static bool wasDown = false, startWasDown = false, startCounts = false;
    static double startAt = 0;
    bool down = false, start = false;
    for (u32 i = 0; i < 8 && SDL_GetKeyboardFocus() != nullptr; ++i) {
        SDL_Gamepad* pad = PADGetSDLGamepadForIndex(i);
        if (pad == nullptr) continue;
        down = down || SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_GUIDE) ||
               (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_BACK) && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_START));
        start = start || SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_START);
    }
    const bool blocked = OnlineMenu::IsOpen() || StartupScreenVisible();
    if (down && !wasDown && !blocked) {
        SetTopBarVisible(!g_topBarVisible);
        g_menu.startHeld = g_topBarVisible && start;  // Back+Start's START is not also a press in the menu
    }
    wasDown = down;
    // A press counts only if it began with the menu closed (the START that closes the menu, still held, does not open it
    // again).
    if (start && !startWasDown) {
        startAt = ImGui::GetTime();
        startCounts = !g_topBarVisible && !down;
    }
    startWasDown = start;
    if (start && startCounts && !down && ImGui::GetTime() - startAt >= 1.0) {
        startCounts = false;
        if (!g_topBarVisible && !blocked && SaveStates::SceneName() != "scMelee") {
            SetTopBarVisible(true);
            g_menu.startHeld = true;
        }
    }
}

// ---- game speed: fast-forward, or the fast menus ----
std::atomic<int> g_gameSpeed{1};

// A scene change is under way (gfSceneManager's next scene set: fading out, loading, fading in), or a screen that only
// shows something (the strap and info screens). Menus a player moves a cursor in keep their speed.
bool TransitionOrInfo(const std::string& scene) {
    constexpr uint32_t kInstanceSlot = RuntimeConfig::SDA1_BASE - 0x43C0u;
    const uint32_t manager = Memory::Read32(kInstanceSlot);
    const bool changing = Memory::Contains(manager, 0x290) && Memory::Read32(manager + 8) != 0;
    return changing || scene == "scStrap" || scene == "scPrizeInfo";
}

// Tests: BRAWL_OPTIONS_TAB=<n> opens the options menu on tab n once the game is up (its title or main menu); BRAWL_STATE_TEST=1 saves 5 s
// into the first match, then loads it at 9 s and 14 s.
void RunTestHooks(const std::string& scene, double sceneSeconds) {
    // A list ("3,5,6") shows each tab for 4 s; "1:9" is tab 1 with its row 9 picked (the list scrolls to it).
    static const char* tabs = std::getenv("BRAWL_OPTIONS_TAB");
    static double openedAt = -1;
    static size_t shown = SIZE_MAX;
    if (tabs != nullptr && !StartupScreenVisible() && (scene == "muMenuMain" || scene == "scTitle")) {
        if (openedAt < 0) openedAt = ImGui::GetTime();
        std::vector<std::pair<int, int>> list;
        for (const char* p = tabs; *p != '\0';) {
            const int tab = std::atoi(p);
            while (*p != '\0' && *p != ',' && *p != ':') ++p;
            int row = 0;
            if (*p == ':') {
                row = std::atoi(++p);
                while (*p != '\0' && *p != ',') ++p;
            }
            list.emplace_back(tab, row);
            if (*p == ',') ++p;
        }
        const size_t i = static_cast<size_t>((ImGui::GetTime() - openedAt) / 4.0);
        if (i < list.size() && i != shown) {
            shown = i;
            g_menu.tab = std::clamp(list[i].first, 0, kTabCount - 1);
            g_menu.inRows = true;
            g_menu.row = list[i].second;
            g_menu.follow = true;
            SetTopBarVisible(true);
            RT_LOGF(RT_TAG_RUNTIME, "[options test] tab %d row %d\n", g_menu.tab, g_menu.row);
        }
    }
    // Measurements: BRAWL_VIDEO_SWEEP="scale:msaa:anisotropy:bloom,..." applies each set for BRAWL_VIDEO_SWEEP_SECONDS
    // (default 8) in turn over the matches' time (past each one's first 3 s), logging when (QueryPerformanceCounter, as
    // PresentMon's --qpc_time) so a GPU timeline can be cut per set.
    static const char* sweep = std::getenv("BRAWL_VIDEO_SWEEP");
    static int sweepStep = -1;
    static double meleeSeconds = 0, lastSceneSeconds = 0;
    if (sweep != nullptr && scene == "scMelee" && sceneSeconds >= 3.0) {
        if (sceneSeconds > lastSceneSeconds && sceneSeconds - lastSceneSeconds < 1.0) meleeSeconds += sceneSeconds - lastSceneSeconds;
        lastSceneSeconds = sceneSeconds;
        static const double seconds = std::getenv("BRAWL_VIDEO_SWEEP_SECONDS") ? std::atof(std::getenv("BRAWL_VIDEO_SWEEP_SECONDS")) : 8.0;
        std::vector<std::string> sets;
        for (const char* p = sweep; *p != '\0';) {
            const char* end = p;
            while (*end != '\0' && *end != ',') ++end;
            sets.emplace_back(p, end);
            p = *end == ',' ? end + 1 : end;
        }
        const int want = static_cast<int>(meleeSeconds / seconds);
        if (want != sweepStep) {
            sweepStep = want;
            uint64_t qpc = 0;
            unsigned long pid = 0;
#if defined(_WIN32)
            LARGE_INTEGER counter{};
            ::QueryPerformanceCounter(&counter);
            qpc = static_cast<uint64_t>(counter.QuadPart);
            pid = ::GetCurrentProcessId();
#endif
            if (want < static_cast<int>(sets.size())) {
                float scale = 2.25f;
                unsigned msaa = 1, aniso = 1, bloom = 1;
                std::sscanf(sets[static_cast<size_t>(want)].c_str(), "%f:%u:%u:%u", &scale, &msaa, &aniso, &bloom);
                SetResolutionScale(scale);
                SetMsaa(msaa);
                SetAnisotropy(aniso);
                if (BloomOn() != (bloom != 0)) SetBloom(bloom != 0);
                RT_LOGF(RT_TAG_RUNTIME, "[video sweep] %d %s qpc %llu pid %lu\n", want,
                        sets[static_cast<size_t>(want)].c_str(), static_cast<unsigned long long>(qpc), pid);
            } else if (want == static_cast<int>(sets.size())) {
                RT_LOGF(RT_TAG_RUNTIME, "[video sweep] end qpc %llu pid %lu\n", static_cast<unsigned long long>(qpc), pid);
            }
        }
    }
    static const bool stateTest = std::getenv("BRAWL_STATE_TEST") != nullptr;
    static int step = 0;
    if (!stateTest || scene != "scMelee") return;
    const double at[3] = {5.0, 9.0, 14.0};
    if (step < 3 && sceneSeconds >= at[step]) {
        RT_LOGF(RT_TAG_RUNTIME, "[savestate test] %s at %.1f s\n", step == 0 ? "save" : "load", sceneSeconds);
        if (step == 0) SaveStates::RequestSave(); else SaveStates::RequestLoad();
        ++step;
    }
}

void UpdateGameSpeed() {
    static std::string lastScene;
    static double sceneStart = 0;
    const std::string scene = SaveStates::SceneName();
    if (scene != lastScene) {
        RT_LOGF(RT_TAG_RUNTIME, "[scene] %s\n", scene.c_str());
        lastScene = scene;
        sceneStart = ImGui::GetTime();
    }
    RunTestHooks(scene, ImGui::GetTime() - sceneStart);
    // #26: a new release is announced once, past the title card (the check runs in the background).
#if !MKW_BRAWL_ONLINE_CORE  // Brawl Online's launcher updates its build
    static bool updateTold = false;
    if (!updateTold && !StartupScreenVisible()) {
        if (const std::string newer = Updates::Available(); !newer.empty()) {
            updateTold = true;
            g_notice = "Brawl Recompiled " + newer + " is out: Options (F10 or Home) > System installs it.";
            g_noticeFrames = 60 * 10;
        }
    }
#endif
    // Loading and scene changes run long frames by nature: they don't count against the frame rate (aurora's pacing).
    // Menus count neither: the stage select reads the saved custom stages as it opens, a one-time hitch that should cost
    // its own frames only, not seconds at 60.
    const bool menu = scene.rfind("mu", 0) == 0 || scene.rfind("scSel", 0) == 0 || scene == "scTitle" || scene == "scBoot" ||
                      scene == "scMemoryChange" || scene.empty();
    aurora_set_frame_pacing_exempt(StartupScreenVisible() || TransitionOrInfo(scene) || menu);
    int speed = 1;
#if !MKW_BRAWL_ONLINE_CORE  // Brawl Online's build: the game runs at its own speed, always (no fast-forward or fast menus)
    if (NetplaySession::Active()) speed = 1;
    else if (g_fastForward.load()) speed = RuntimeConfigFile::FastForwardSpeed();
    else if (RuntimeConfigFile::FastMenus() && TransitionOrInfo(scene)) speed = 3;
#endif
    g_gameSpeed.store(speed, std::memory_order_relaxed);
}
