// settings_video.inc - how the game looks: quality presets, anti-aliasing (MSAA), anisotropic filtering, V-Sync's
// automatic setting (off, unless a G-Sync / FreeSync screen would tear; display_refresh.h) and the picture adjustments drawn in the
// present pass (sharpening, brightness, contrast, vibrance). All of it is display only: nothing here reaches the
// game's memory, so online play is the same whatever each player picks. Included by settings_overlay.cpp inside its
// anonymous namespace, after the graphics state it shares (resolution, bloom, frame rate) and before
// settings_menu.inc, whose Graphics tab shows these rows.

uint32_t g_msaa = RuntimeConfigFile::Msaa(1);
uint32_t g_anisotropy = RuntimeConfigFile::Anisotropy(1);
int g_sharpening = RuntimeConfigFile::Sharpening(0);
int g_brightness = RuntimeConfigFile::Brightness(0);
int g_contrast = RuntimeConfigFile::Contrast(0);
int g_vibrance = RuntimeConfigFile::Vibrance(0);
bool g_vsyncAuto = RuntimeConfigFile::VSyncAuto();

// Each preset sets these together; any other combination is Custom. The frame rate is the player's (it follows the
// screen): Medium, High and Ultra keep frames above 60 and make them with Predict, the sub-frame mode that adds no
// delay (card 771); Low goes back to 60, as each inserted frame is drawn again.
struct QualityPreset {
    const char* name;
    float scale;  // resolution multiplier (kResolutions)
    uint32_t msaa;
    uint32_t anisotropy;
    bool bloom;
    bool aboveSixty;  // keeps a frame rate above 60 (with Predict)
};
constexpr std::array<QualityPreset, 4> kQualityPresets = {{
    {"LOW", 1.0f, 1, 1, false, false},
    {"MEDIUM", 1.5f, 1, 4, true, true},
    {"HIGH", 2.25f, 4, 4, true, true},
    {"ULTRA", 4.5f, 4, 16, true, true},
}};
constexpr int kQualityPresetHigh = 2;
constexpr std::array<uint32_t, 4> kMsaaChoices = {1, 2, 4, 8};
constexpr std::array<uint32_t, 5> kAnisotropyChoices = {1, 2, 4, 8, 16};

bool BloomOn() { return (g_disabledPostProcessingPaths & 0x10u) == 0; }

void SetBloom(bool on) {
    g_disabledPostProcessingPaths = on ? (g_disabledPostProcessingPaths & ~0x10u) : (g_disabledPostProcessingPaths | 0x10u);
    RuntimeGameGraphicsOptions::SetDisabledPostProcessingPaths(g_disabledPostProcessingPaths);
    RuntimeConfigFile::SetDisabledPostProcessingPaths(g_disabledPostProcessingPaths);
}

void SetMsaa(uint32_t samples) {
    g_msaa = samples;
    aurora_set_msaa(samples);
    RuntimeConfigFile::SetMsaa(samples);
}

void SetAnisotropy(uint32_t value) {
    g_anisotropy = value;
    aurora_set_texture_anisotropy(value);
    RuntimeConfigFile::SetAnisotropy(value);
}

// A preset's resolution at the current frame rate (above 60, 4K is the most: LimitResolutionForFrameRate).
float PresetScale(const QualityPreset& p) {
    return IsHighFrameRateMode() && IsHighResolutionScale(p.scale) ? 4.0f : p.scale;
}

// The preset the current settings are, or -1 (Custom).
int CurrentQualityPreset() {
    for (size_t i = 0; i < kQualityPresets.size(); ++i) {
        const QualityPreset& p = kQualityPresets[i];
        if (g_frameInterpolationMode != 0 && !(p.aboveSixty && g_framePredict)) continue;
        if (std::fabs(g_resolutionScale - PresetScale(p)) < 0.001f && g_msaa == p.msaa && g_anisotropy == p.anisotropy &&
            BloomOn() == p.bloom) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void ApplyQualityPreset(int index) {
    const QualityPreset& p = kQualityPresets[static_cast<size_t>(std::clamp(index, 0, static_cast<int>(kQualityPresets.size()) - 1))];
    SetResolutionScale(p.scale);
    SetMsaa(p.msaa);
    SetAnisotropy(p.anisotropy);
    if (BloomOn() != p.bloom) SetBloom(p.bloom);
    if (g_frameInterpolationMode != 0 && !p.aboveSixty) {
        g_frameInterpolationMode = 0;
        aurora_set_frame_interpolation_fps(0);
        RuntimeConfigFile::SetFrameInterpolationFps(0);
    } else if (g_frameInterpolationMode != 0) {
        g_framePredict = true;
        aurora_set_frame_interpolation_mode(AURORA_FRAME_INTERPOLATION_PREDICT);
        RuntimeConfigFile::SetFrameInterpolationMode("predict");
        LimitResolutionForFrameRate();
    }
}

// A first start's preset: Low on an integrated GPU or one with little memory of its own, else High.
int FirstStartQualityPreset() {
    const uint64_t memory = GraphicsInfo::MemoryBytes();
    if (GraphicsInfo::Integrated() || (memory != 0 && memory < (3ull << 30))) return 0;
    return kQualityPresetHigh;
}

void ApplyPictureAdjustments() {
    aurora_set_picture_adjustments(static_cast<float>(g_sharpening) / 100.0f, 1.0f + static_cast<float>(g_brightness) / 100.0f,
                                   1.0f + static_cast<float>(g_contrast) / 100.0f, 1.0f + static_cast<float>(g_vibrance) / 100.0f);
}

// At start, after the GPU is known (InitializeRuntimeSettings).
void InitializeVideoSettings() {
    if (!RuntimeConfigFile::HasQualitySettings()) {
        const int preset = FirstStartQualityPreset();
        ApplyQualityPreset(preset);
        RT_LOGF(RT_TAG_CONFIG, "first start: quality %s for %s (%s, %llu MiB)\n", kQualityPresets[static_cast<size_t>(preset)].name,
                GraphicsInfo::Name().c_str(), GraphicsInfo::Integrated() ? "integrated" : "discrete",
                static_cast<unsigned long long>(GraphicsInfo::MemoryBytes() >> 20));
    } else {
        aurora_set_msaa(g_msaa);
        aurora_set_texture_anisotropy(g_anisotropy);
    }
    ApplyPictureAdjustments();
    const int preset = CurrentQualityPreset();
    RT_LOGF(RT_TAG_CONFIG, "video: quality %s, %.2fx, MSAA %ux, anisotropy %ux, bloom %s, V-Sync %s, sharpening %d%%\n",
            preset >= 0 ? kQualityPresets[static_cast<size_t>(preset)].name : "CUSTOM", g_resolutionScale, g_msaa, g_anisotropy,
            BloomOn() ? "on" : "off", g_vsyncAuto ? "auto" : (g_vsync.load() ? "on" : "off"), g_sharpening);
}

// V-Sync's automatic setting, each frame, on every platform: off, so presents never wait for a refresh (Brawl Online
// board: "if gsync is not detected, vsync should default to OFF, on all playforms for the recomp"). The one time it
// goes on is a screen that follows the frames (G-Sync, FreeSync) when they would come at least as fast as its
// refresh: past that the screen tears like a fixed one. Detection is Windows only (display_refresh.h), so on macOS
// and Linux auto is always off.
void UpdateAutoVSync() {
    if (!g_vsyncAuto) return;
    const double hz = DisplayRefresh::RefreshHz();
    const uint32_t target = kFrameInterpolationTargetFps[static_cast<size_t>(g_frameInterpolationMode)];
    const double fps = target != 0 ? static_cast<double>(target) : 60.0;
    const bool on = DisplayRefresh::VariableRefresh() == 1 && hz > 0 && fps >= 0.96 * hz;
    if (on != g_vsync.load()) {
        g_vsync.store(on);
        aurora_set_vsync(on);
        RT_LOGF(RT_TAG_CONFIG, "V-Sync (automatic): %s\n", on ? "on, the frames reach the G-Sync screen's refresh" : "off");
    }
}

// ---- rows of the options menu's Graphics tab ----
void QualityRow(std::vector<MenuRow>& rows) {
    const int preset = CurrentQualityPreset();
    rows.push_back({"Quality", preset >= 0 ? kQualityPresets[static_cast<size_t>(preset)].name : "CUSTOM",
                    "Sets resolution, anti-aliasing, texture filtering and bloom together.", [preset](int step) {
                        const int count = static_cast<int>(kQualityPresets.size());
                        const int next = preset < 0 ? (step < 0 ? count - 1 : 0) : Cycle(preset, step, count);
                        ApplyQualityPreset(next);
                    }});
}

void AntiAliasingRows(std::vector<MenuRow>& rows) {
    static constexpr const char* kMsaaNames[] = {"OFF", "2X MSAA", "4X MSAA", "8X MSAA"};
    int msaa = 0;
    for (size_t i = 0; i < kMsaaChoices.size(); ++i) if (kMsaaChoices[i] == g_msaa) msaa = static_cast<int>(i);
    rows.push_back({"Anti-aliasing", kMsaaNames[msaa],
                    "Reduces the amount of aliasing caused by rasterizing 3D graphics, resulting in smoother edges on "
                    "objects. Increases GPU load.",
                    [msaa](int step) { SetMsaa(kMsaaChoices[static_cast<size_t>(Cycle(msaa, step, static_cast<int>(kMsaaChoices.size())))]); }});
    static constexpr const char* kAnisotropyNames[] = {"1X", "2X", "4X", "8X", "16X"};
    int aniso = 0;
    for (size_t i = 0; i < kAnisotropyChoices.size(); ++i) if (kAnisotropyChoices[i] == g_anisotropy) aniso = static_cast<int>(i);
    rows.push_back({"Anisotropic filtering", kAnisotropyNames[aniso],
                    "Anisotropic filtering enhances the visual quality of textures that are at oblique viewing angles.",
                    [aniso](int step) {
                        SetAnisotropy(kAnisotropyChoices[static_cast<size_t>(Cycle(aniso, step, static_cast<int>(kAnisotropyChoices.size())))]);
                    }});
}

void VSyncRow(std::vector<MenuRow>& rows) {
    // AUTO -> ON -> OFF. Auto shows what it chose (UpdateAutoVSync): G-SYNC when off on a screen that follows the
    // frames, OFF when off on any other, ON on a G-Sync screen the frames would outrun.
    const int mode = g_vsyncAuto ? 0 : (g_vsync.load() ? 1 : 2);
    const char* autoValue = g_vsync.load() ? "AUTO: ON" : (DisplayRefresh::VariableRefresh() == 1 ? "AUTO: G-SYNC" : "AUTO: OFF");
    const std::string value = g_vsyncAuto ? autoValue : OnOff(g_vsync.load());
    rows.push_back({"V-Sync", value, "No screen tearing; the game follows your screen's refresh.",
                    [mode](int step) {
                        const int next = Cycle(mode, step, 3);
                        if (next == 0) {
                            g_vsyncAuto = true;
                            RuntimeConfigFile::SetVSyncAuto();
                            UpdateAutoVSync();
                        } else {
                            g_vsyncAuto = false;
                            SetVSync(next == 1);
                        }
                    }});
}

void PictureRows(std::vector<MenuRow>& rows) {
    rows.push_back({"Sharpening", std::to_string(g_sharpening) + "%", "Left and right change it.", [](int step) {
                        if (step == 0) return;
                        g_sharpening = std::clamp(g_sharpening + step * 10, 0, 100);
                        ApplyPictureAdjustments();
                        RuntimeConfigFile::SetSharpening(g_sharpening);
                    }, true, g_sharpening, [](int value, bool letGo) {
                        g_sharpening = std::clamp(value, 0, 100);
                        ApplyPictureAdjustments();
                        if (letGo) RuntimeConfigFile::SetSharpening(g_sharpening);
                    }});
    const auto adjustRow = [&rows](const char* label, int& value, bool (*save)(int32_t)) {
        rows.push_back({label, value > 0 ? "+" + std::to_string(value) : std::to_string(value), "Left and right change it.",
                        [&value, save](int step) {
                            if (step == 0) return;
                            value = std::clamp(value + step * 5, -50, 50);
                            ApplyPictureAdjustments();
                            save(value);
                        }, true, value + 50, [&value, save](int position, bool letGo) {
                            value = std::clamp(position - 50, -50, 50);
                            ApplyPictureAdjustments();
                            if (letGo) save(value);
                        }});
        rows.back().centered = true;
    };
    adjustRow("Brightness", g_brightness, RuntimeConfigFile::SetBrightness);
    adjustRow("Contrast", g_contrast, RuntimeConfigFile::SetContrast);
    adjustRow("Vibrance", g_vibrance, RuntimeConfigFile::SetVibrance);
}
