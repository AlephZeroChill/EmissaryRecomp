#include "brawl_online_core.h"
#include "det_clock.h"
#include "display_settings.h"

#include "memory.h"
#include "runtime_log.h"

#include <dolphin/gx/GXAurora.h>
#include <dolphin/vi.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <algorithm>
#include <mutex>

namespace DisplaySettings {
namespace {

constexpr float kLinesPerScale = 480.0f;
constexpr float kRatio16x9 = 16.0f / 9.0f;

std::mutex g_mutex;
Settings g_current;
Settings g_pending;
bool g_hasPending = false;
bool g_bootWidescreen = true;
bool g_rendererDirty = true;
uint32_t g_surfaceWidth = 0;
uint32_t g_surfaceHeight = 0;

// The game booted in 4:3: neither widescreen mode exists until the next start.
int32_t EffectiveAspect(int32_t aspect) {
    return g_bootWidescreen ? aspect : kAspect4x3;
}

// Brawl Online's core draws any screen shape itself (PictureAspect): the game's picture is shown at the shape the
// player picked, stretched to it, and aurora's own widening stays off (the plugin rescales GX's projection instead,
// the same on Dolphin and here, so both draw the same pictures and the cameras stay the match's).
bool CoreDrawsShape() {
    static const bool on = BrawlOnlineCore::Enabled();
    return on;
}

bool SurfaceWiderThan169() {
    return g_surfaceWidth != 0 && g_surfaceHeight != 0 &&
           static_cast<float>(g_surfaceWidth) / static_cast<float>(g_surfaceHeight) > kRatio16x9;
}

// Render scale 0 ("match screen resolution"): exactly as many lines as the picture is tall on the
// surface, so it renders 1:1 (a 2560x1080 surface in Fill renders 2560x1080, with the game's view
// widened to match), not the window size with the EFB's workspace rows squeezed in.
float RenderLines(float renderScale, int32_t aspect) {
    if (renderScale > 0.0f || g_surfaceWidth == 0 || g_surfaceHeight == 0) {
        return LinesForScale(renderScale);
    }
    const float w = static_cast<float>(g_surfaceWidth), h = static_cast<float>(g_surfaceHeight);
    const bool fillsSurface = aspect == kAspectFill && (SurfaceWiderThan169() || CoreDrawsShape());
    const float pictureAspect = aspect == kAspect4x3 ? 4.0f / 3.0f : aspect == kAspectCinematic ? kRatioCinematic : kRatio16x9;
    return fillsSurface ? h : std::min(h, w / pictureAspect);
}

void ApplyRenderer(const Settings& settings) {
    const int32_t aspect = EffectiveAspect(settings.aspect);
    if (aspect == kAspect4x3) {
        AuroraSetViewportPolicy(AURORA_VIEWPORT_FIT);
        VILockAspectRatio(4, 3);
    } else if (aspect == kAspectCinematic) {
        // A 2.39:1 picture, letterboxed on narrower screens; the game draws the wider view into it.
        VIUnlockAspectRatio();
        AuroraSetViewportFixedAspect(kRatioCinematic);
    } else if (aspect == kAspect16x9 || (!SurfaceWiderThan169() && !CoreDrawsShape())) {
        // 16:9, or Fill on a surface no wider than 16:9 (the picture is never squashed).
        VIUnlockAspectRatio();
        AuroraSetViewportPolicy(AURORA_VIEWPORT_16_9);
    } else {
        // The framebuffer takes the surface's aspect; the game draws the wider view into it.
        VIUnlockAspectRatio();
        AuroraSetViewportPolicy(AURORA_VIEWPORT_STRETCH);
    }
    VISetFrameBufferLines(RenderLines(settings.renderScale, aspect));
    RT_LOGF(RT_TAG_VI, "display: aspect %d%s, render %.0f lines (%.2gx), surface %ux%u\n", aspect,
            aspect != settings.aspect ? " (game booted in 4:3)" : "", RenderLines(settings.renderScale, aspect),
            settings.renderScale, g_surfaceWidth, g_surfaceHeight);
}

// Fill: widen the game's 16:9 canvas to the surface (Hor+), keeping the game's own pixel aspect
// (its 832 units stand for 16:9). Otherwise keep, or put back, the game's own width.
void ApplyGameCanvas(int32_t aspect) {
    // 3D: cameras set up for 16:9 are widened to the screen (aurora's projection), the same view
    // height with more to the sides, instead of being stretched across it.
    const bool widened = aspect == kAspectFill && SurfaceWiderThan169() && !CoreDrawsShape();
    AuroraSetWidePerspectiveAspect(
        CoreDrawsShape() ? 0.0f
        : aspect == kAspectCinematic ? kRatioCinematic
        : widened ? static_cast<float>(g_surfaceWidth) / static_cast<float>(g_surfaceHeight) : 0.0f);

}

} // namespace

float LinesForScale(float renderScale) {
    return renderScale > 0.0f ? kLinesPerScale * renderScale : 0.0f;
}

// Deterministic mode (network play) keeps every device on the same 16:9 canvas: how wide the game
// draws decides which enemies it wakes, so Fill's screen-dependent width would desync the session.
Settings Lockstep(Settings settings) {
    if (DetClock::Enabled() && !CoreDrawsShape()) {
        settings.aspect = kAspect16x9;
    }
    return settings;
}

void Initialize(const Settings& requested) {
    const Settings settings = Lockstep(requested);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_current = settings;
    // The game reads its aspect (SCGetAspectRatio) once, at boot, and frames its cameras, and so a screen KO, by it.
    // With Brawl Online's core every machine boots it widescreen, as Dolphin pins SYSCONF IPL.AR on (brawlonline.net
    // 18e14c7a: a Wii at 4:3 desynced at the first screen KO): a 4:3 picture is then the plugin's (PictureAspect), as
    // Dolphin's Graphics > Aspect Ratio. The launcher writes a widescreen aspect_mode before each start, but a 4:3 one
    // picked in game was saved and read back by any start it didn't make.
    g_bootWidescreen = CoreDrawsShape() || settings.aspect != kAspect4x3;
    g_rendererDirty = true;
}

void Request(const Settings& settings) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_pending = Lockstep(settings);
    g_hasPending = true;
}

void OnFrameBoundary(uint32_t surfaceWidth, uint32_t surfaceHeight) {
    Settings settings;
    bool renderer = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_hasPending) {
            g_current = g_pending;
            g_hasPending = false;
            g_rendererDirty = true;
        }
        if (surfaceWidth != 0 && surfaceHeight != 0 &&
            (surfaceWidth != g_surfaceWidth || surfaceHeight != g_surfaceHeight)) {
            const bool wasWide = SurfaceWiderThan169();
            g_surfaceWidth = surfaceWidth;
            g_surfaceHeight = surfaceHeight;
            // Fill switches between stretching and the 16:9 frame as the surface crosses 16:9, and
            // a "match screen" render follows every size.
            if (SurfaceWiderThan169() != wasWide || g_current.renderScale <= 0.0f) {
                g_rendererDirty = true;
            }
        }
        settings = g_current;
        renderer = g_rendererDirty;
        g_rendererDirty = false;
    }
    if (renderer) {
        ApplyRenderer(settings);
    }
    ApplyGameCanvas(EffectiveAspect(settings.aspect));
}

Settings Current() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_current;
}

Settings Latest() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_hasPending ? g_pending : g_current;
}

bool GameWidescreen() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_bootWidescreen;
}

bool AspectNeedsRestart(int32_t aspect) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return !CoreDrawsShape() && (aspect != kAspect4x3) != g_bootWidescreen;
}

float PictureAspect() {
    std::lock_guard<std::mutex> lock(g_mutex);
    switch (EffectiveAspect(g_current.aspect)) {
    case kAspect4x3: return 4.0f / 3.0f;
    case kAspectCinematic: return kRatioCinematic;
    case kAspectFill:
        return g_surfaceWidth && g_surfaceHeight ? static_cast<float>(g_surfaceWidth) / static_cast<float>(g_surfaceHeight)
                                                 : 0.0f;
    default: return kRatio16x9;
    }
}

bool SurfaceWide() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return SurfaceWiderThan169();
}

} // namespace DisplaySettings
