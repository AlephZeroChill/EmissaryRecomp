#include "brawl_dirty_memory.h"
#include "hle_stubs.h"
#include "memory.h"
#include "hle/controller_status_contract.h"
#include "input_bindings.h"
#include "wii_remote_input.h"
#include "frame_input.h"
#include "brawl_online_core.h"
#include "runtime_config.h"
#include "save_states.h"
#include "settings_overlay.h"
#include "pplus_si_codes.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keyboard.h>
#include <aurora/aurora.h>
#include <dolphin/pad.h>

extern "C" int g_gxFrameCount;  // gx_utils.cpp: display copies made

// LLVM's profile runtime (present only in -fprofile-instr-generate builds); see the pad script's EXIT.
extern "C" int __llvm_profile_write_file(void) __attribute__((weak));

namespace {

std::atomic<bool> g_rumbleEnabled{true};

bool NativeButtonHeld(SDL_Gamepad* gamepad, uint32_t nativeButton) {
    if (gamepad == nullptr || nativeButton == PAD_NATIVE_BUTTON_INVALID ||
        nativeButton >= SDL_GAMEPAD_BUTTON_COUNT) {
        return false;
    }
    return SDL_GetGamepadButton(gamepad, static_cast<SDL_GamepadButton>(nativeButton));
}

// A digital button bound to L or R has no analog travel of its own. On real
// hardware the click only engages at full depression, so report a full pull.
void FillTriggersHeldByButtons(PADStatus* statuses) {
    if (InputBindings::InputBlocked()) {
        return;
    }
    for (uint32_t port = 0; port < PAD_CHANMAX; ++port) {
        if (statuses[port].err != PAD_ERR_NONE) {
            continue;
        }
        const s32 index = PADGetIndexForPort(port);
        if (index < 0) {
            continue;
        }
        SDL_Gamepad* gamepad = PADGetSDLGamepadForIndex(static_cast<u32>(index));
        if (gamepad == nullptr) {
            continue;
        }
        const auto scan = [&](PADButtonMapping* mappings, u32 count) {
            if (mappings == nullptr) {
                return;
            }
            for (u32 i = 0; i < count; ++i) {
                const PADButtonMapping& mapping = mappings[i];
                if (mapping.padButton != PAD_TRIGGER_L && mapping.padButton != PAD_TRIGGER_R) {
                    continue;
                }
                if (!NativeButtonHeld(gamepad, mapping.nativeButton)) {
                    continue;
                }
                if (mapping.padButton == PAD_TRIGGER_L) {
                    statuses[port].triggerLeft = 255;
                } else {
                    statuses[port].triggerRight = 255;
                }
            }
        };
        u32 count = 0;
        scan(PADGetButtonMappings(port, &count), count);
        count = 0;
        scan(PADGetAltButtonMappings(port, &count), count);
    }
}

void WritePadStatus(uint32_t base, const PADStatus& status) {
    const auto guestStatus = PadStatusContract::Encode({
        status.button,
        status.stickX,
        status.stickY,
        status.substickX,
        status.substickY,
        status.triggerL,
        status.triggerR,
        status.analogA,
        status.analogB,
        status.err,
    });
    uint8_t* dst = Memory::GetPointer(base, guestStatus.size());
    BrawlDirtyMemory::Mark(base,guestStatus.size());
    std::memcpy(dst, guestStatus.data(), guestStatus.size());
}

} // namespace

extern "C" void PAD_HLE_SetRumbleEnabled(bool enabled)
{
    g_rumbleEnabled.store(enabled, std::memory_order_relaxed);
}

namespace {

// The launcher's keyboard mapping (Config.toml [gc_keyboard]): enabled, then one key per GameCube button and stick
// direction, by SDL scancode name ("Space", "Left Shift") or "Mouse Left" / "Mouse Middle" / "Mouse Right". The
// keyboard then plays port 1, as the in-game menu's "Keyboard and mouse" switch does.
int32_t KeyFromName(const std::string& name) {
    if (name == "Mouse Left") return PAD_KEY_MOUSE_LEFT;
    if (name == "Mouse Middle") return PAD_KEY_MOUSE_MIDDLE;
    if (name == "Mouse Right") return PAD_KEY_MOUSE_RIGHT;
    const SDL_Scancode code = SDL_GetScancodeFromName(name.c_str());
    return code == SDL_SCANCODE_UNKNOWN ? PAD_KEY_INVALID : static_cast<int32_t>(code);
}

void ApplyKeyboardConfig() {
    toml::value table;
    try {
        std::ifstream in(RuntimeConfigFile::ResolveConfigPath(), std::ios::binary);
        if (!in) return;
        table = toml::parse(in, "Config.toml");
    } catch (const std::exception&) {
        return;  // the game's own config reader reports a broken Config.toml
    }
    const auto enabled = RuntimeConfigFile::FindConfigValue<bool>(table, "gc_keyboard", "enabled");
    if (!enabled) return;  // no launcher setting: the in-game menu's own choice stands
    u32 loaded = 0;
    PADGetKeyButtonBindings(0, &loaded);  // aurora reads its saved bindings on first use: before ours, not over them
    PADSetKeyboardActive(0, *enabled ? TRUE : FALSE);
    if (*enabled) {
        static constexpr std::pair<const char*, PADButton> kButtons[] = {
            {"a", PAD_BUTTON_A}, {"b", PAD_BUTTON_B}, {"x", PAD_BUTTON_X}, {"y", PAD_BUTTON_Y},
            {"start", PAD_BUTTON_START}, {"z", PAD_TRIGGER_Z}, {"l", PAD_TRIGGER_L}, {"r", PAD_TRIGGER_R},
            {"dpad_up", PAD_BUTTON_UP}, {"dpad_down", PAD_BUTTON_DOWN}, {"dpad_left", PAD_BUTTON_LEFT},
            {"dpad_right", PAD_BUTTON_RIGHT}};
        static constexpr std::pair<const char*, PADAxis> kAxes[] = {
            {"stick_right", PAD_AXIS_LEFT_X_POS}, {"stick_left", PAD_AXIS_LEFT_X_NEG},
            {"stick_up", PAD_AXIS_LEFT_Y_POS}, {"stick_down", PAD_AXIS_LEFT_Y_NEG},
            {"cstick_right", PAD_AXIS_RIGHT_X_POS}, {"cstick_left", PAD_AXIS_RIGHT_X_NEG},
            {"cstick_up", PAD_AXIS_RIGHT_Y_POS}, {"cstick_down", PAD_AXIS_RIGHT_Y_NEG}};
        for (const auto& [key, button] : kButtons) {
            if (const auto name = RuntimeConfigFile::FindConfigValue<std::string>(table, "gc_keyboard", key))
                PADSetKeyButtonBinding(0, {KeyFromName(*name), button});
        }
        for (const auto& [key, axis] : kAxes) {
            if (const auto name = RuntimeConfigFile::FindConfigValue<std::string>(table, "gc_keyboard", key))
                PADSetKeyAxisBinding(0, {KeyFromName(*name), axis, 1});
        }
    }
    PADSerializeMappings();
}

} // namespace

extern "C" uint32_t PAD__Init_HLE()
{
    const bool ok = PADInit();
    ApplyKeyboardConfig();
    return ok ? 1u : 0u;
}
// Brawl (RSBE01): GameCube controllers are its main input.
PPC_NATIVE_OVERRIDE(80215E68, PAD__Init_HLE, uint32_t, (), ());

// Every GameCube pad source for this poll, as the game would read them: SDL pads, the keyboard, bindings, and a
// test script last.
static uint32_t ReadLocalPads(PADStatus* statuses)
{
    // Keep looking for a Bluetooth Wii Remote that dropped out (or was turned on late).
    WiiRemoteInput::Poll();
    uint32_t rumbleMask = PADRead(statuses);
    // Wii Remotes reach the game through KPAD, not as GameCube pads. This also
    // applies while input is blocked (overlay open) so the port does not flip
    // between "connected" and "no controller" every time the overlay toggles.
    WiiRemoteInput::HideRemotesFromPad(statuses, PAD_CHANMAX);

    FillTriggersHeldByButtons(statuses);
    InputBindings::Apply(statuses);
    // Brawl Online: the ports as Dolphin's Controllers settings have them (when the player set them up there).
    BrawlOnlineCore::ApplyDolphinControls(statuses);
    // Test scripts last, and alone: a key or mouse button pressed at the PC during a scripted run must not reach
    // the game (the keyboard pad maps the mouse buttons to A and B).
    // A script's channel field drives that port (a second player's picks in a scripted match).
    for (uint32_t port = 0; port < PAD_CHANMAX; ++port) {
        uint16_t scripted = 0;
        int8_t stickX = 0, stickY = 0;
        if (!WiiRemoteInput::ScriptedPadButtons(port, scripted, stickX, stickY)) continue;
        statuses[port] = {};
        statuses[port].err = PAD_ERR_NONE;
        statuses[port].button = scripted;
        statuses[port].stickX = stickX;
        statuses[port].stickY = stickY;
    }
    return rumbleMask;
}

namespace {
void ApplyPadScript(PADStatus& port1);
}

// Brawl Online's looks at the local controllers outside the game's reads (input suggestions between them, the core's
// read after a rollback): the ports as ReadLocalPads would make them now, changing nothing (aurora's PADPeek, no Wii
// Remote scan, no script step), after SDL has read its controllers (the event pump does that once a frame). False
// while a test script drives the pads (its steps count the game's reads). The game's thread.
bool PAD_HLE_PeekLocalPads(FrameInput::GcPad* pads)
{
    if (WiiRemoteInput::ScriptActive() || std::getenv("BRAWL_PAD_SCRIPT") != nullptr) return false;
    SDL_UpdateJoysticks();
    PADStatus statuses[PAD_CHANMAX]{};
    PADPeek(statuses);
    WiiRemoteInput::HideRemotesFromPad(statuses, PAD_CHANMAX);
    FillTriggersHeldByButtons(statuses);
    InputBindings::Apply(statuses, true);
    BrawlOnlineCore::ApplyDolphinControls(statuses, true);
    for (uint32_t port = 0; port < PAD_CHANMAX; ++port) {
        const PADStatus& s = statuses[port];
        pads[port] = {s.err == PAD_ERR_NONE, s.button, s.stickX, s.stickY, s.substickX, s.substickY,
                      s.triggerLeft, s.triggerRight};
    }
    return true;
}

// Deterministic mode (network play): the local ports for frame_input.cpp to latch at the retrace. A pad script
// (BRAWL_PAD_SCRIPT) drives port 1 here too, once per latched frame, so a standalone deterministic run
// (NSMBW_DETERMINISTIC=1) gets past the strap screen; PADRead takes the latched frame and never sees the script.
void PAD_HLE_SampleLocalPads(FrameInput::GcPad* pads)
{
    PADStatus statuses[PAD_CHANMAX]{};
    ReadLocalPads(statuses);
    ApplyPadScript(statuses[0]);
    for (uint32_t port = 0; port < PAD_CHANMAX; ++port) {
        const PADStatus& s = statuses[port];
        pads[port] = {s.err == PAD_ERR_NONE, s.button, s.stickX, s.stickY, s.substickX, s.substickY,
                      s.triggerLeft, s.triggerRight};
    }
}

// BRAWL_PAD_SCRIPT="frame:BUTTONS[:hold];...": port 1 presses at fixed game frames (counted in PADRead calls), so a
// test reaches the same screen in every run whatever the frame rate. BUTTONS joins A B X Y Z L R START UP DOWN LEFT
// RIGHT (the D-pad) and SUP SDOWN SLEFT SRIGHT (the control stick held fully) with '+'; hold is in frames (default 6).
// "frame:EXIT" ends the process there, so a measurement run stops as soon as it has what it came for.
#if defined(__SWITCH__)
extern "C" void HorizonExitNow();
#else
// MKW_EXIT_AFTER_SECONDS=N (native): end the process N seconds after start the way a script's EXIT does, from a
// timer thread, so a run that hangs (a stage load that never finishes) still ends from inside instead of being
// killed mid-render from outside (that has hung the host GPU). Horizon has HZ_EXIT_AFTER for the same purpose.
[[maybe_unused]] static const bool g_exitAfterArmed = [] {
    const char* text = std::getenv("MKW_EXIT_AFTER_SECONDS");
    const long seconds = text ? std::strtol(text, nullptr, 10) : 0;
    if (seconds <= 0) return false;
    std::thread([seconds] {
        std::this_thread::sleep_for(std::chrono::seconds(seconds));
        std::fprintf(stderr, "[padscript] MKW_EXIT_AFTER_SECONDS=%ld reached: exiting\n", seconds);
        std::fflush(stderr);
        std::_Exit(3);
    }).detach();
    return true;
}();
#endif
namespace {
// A press can also be timed from a scene: "muMenuMain+40:A" is 40 frames after that scene first appears (which
// screens a boot shows first varies, so frame numbers alone drift).
struct PadScriptPress {
    uint64_t frame;
    uint32_t hold;
    uint16_t buttons;
    int8_t stickX, stickY;
    std::string text;
    bool exit = false;
    // Diagnostics, at the script's clock (the same game frames however the host runs): SHOT saves the next rendered
    // frame as shot_<item>.bmp; DUMP writes the next whole frame's draws to <AURORA_DRAW_DUMP>/<item>.txt.
    bool shot = false;
    bool dump = false;
    bool memdump = false;  // MEMDUMP: BRAWL_DUMP_RANGES' guest memory ranges, now (see DumpGuestRanges)
    std::string scene;  // empty: frame counts from boot
    // "scene*N:BUTTONS": press every N frames for as long as that scene is the current one, so a screen that drops
    // one press (still fading in) gets the next, and the script moves on only when the game has.
    uint32_t every = 0;
};

std::vector<PadScriptPress> ParsePadScript(const char* text) {
    std::vector<PadScriptPress> presses;
    std::string all(text);
    size_t start = 0;
    while (start < all.size()) {
        size_t end = all.find(';', start);
        if (end == std::string::npos) end = all.size();
        const std::string item = all.substr(start, end - start);
        start = end + 1;
        const size_t colon = item.find(':');
        if (colon == std::string::npos) continue;
        PadScriptPress press{std::strtoull(item.c_str(), nullptr, 10), 6, 0, 0, 0, item};
        if (!item.empty() && (item[0] < '0' || item[0] > '9')) {
            const std::string when = item.substr(0, colon);
            const size_t star = when.find('*');
            const size_t plus = when.find('+');
            press.scene = when.substr(0, std::min(star, plus));
            if (star != std::string::npos) {
                press.every = static_cast<uint32_t>(std::strtoul(when.c_str() + star + 1, nullptr, 10));
                if (press.every < 2) press.every = 2;
                press.hold = std::min(press.hold, press.every / 2);  // released between presses, or it is one long hold
            } else {
                press.frame = plus == std::string::npos ? 0 : std::strtoull(when.c_str() + plus + 1, nullptr, 10);
            }
        }
        std::string names = item.substr(colon + 1);
        if (const size_t holdColon = names.find(':'); holdColon != std::string::npos) {
            press.hold = static_cast<uint32_t>(std::strtoul(names.c_str() + holdColon + 1, nullptr, 10));
            names.resize(holdColon);
        }
        size_t n = 0;
        while (n <= names.size()) {
            size_t plus = names.find('+', n);
            if (plus == std::string::npos) plus = names.size();
            const std::string name = names.substr(n, plus - n);
            n = plus + 1;
            if (name == "A") press.buttons |= PAD_BUTTON_A;
            else if (name == "B") press.buttons |= PAD_BUTTON_B;
            else if (name == "X") press.buttons |= PAD_BUTTON_X;
            else if (name == "Y") press.buttons |= PAD_BUTTON_Y;
            else if (name == "Z") press.buttons |= PAD_TRIGGER_Z;
            else if (name == "L") press.buttons |= PAD_TRIGGER_L;
            else if (name == "R") press.buttons |= PAD_TRIGGER_R;
            else if (name == "START") press.buttons |= PAD_BUTTON_START;
            else if (name == "UP") press.buttons |= PAD_BUTTON_UP;
            else if (name == "DOWN") press.buttons |= PAD_BUTTON_DOWN;
            else if (name == "LEFT") press.buttons |= PAD_BUTTON_LEFT;
            else if (name == "RIGHT") press.buttons |= PAD_BUTTON_RIGHT;
            else if (name == "SUP") press.stickY = 127;
            else if (name == "SDOWN") press.stickY = -128;
            else if (name == "SLEFT") press.stickX = -128;
            else if (name == "SRIGHT") press.stickX = 127;
            else if (name == "EXIT") press.exit = true;
            else if (name == "SHOT") press.shot = true;
            else if (name == "DUMP") press.dump = true;
            else if (name == "MEMDUMP") press.memdump = true;
        }
        presses.push_back(press);
    }
    return presses;
}

// "START-END,START-END:prefix": guest memory ranges (hex) written to prefix_START.bin (a mod's code as it stands then,
// for gen_variant_spec.py / bake_patches.py). BRAWL_DUMP_ON_SCENE dumps them when a scene first comes up; a script's
// MEMDUMP press dumps BRAWL_DUMP_RANGES' at its frame (modules the scene loads after its first frame).
void DumpGuestRanges(const std::string& spec) {
    const size_t last = spec.rfind(':');
    if (last == std::string::npos) return;
    const std::string ranges = spec.substr(0, last), prefix = spec.substr(last + 1);
    size_t at = 0;
    while (at < ranges.size()) {
        size_t comma = ranges.find(',', at);
        if (comma == std::string::npos) comma = ranges.size();
        char* dash = nullptr;
        const uint32_t start = static_cast<uint32_t>(std::strtoul(ranges.c_str() + at, &dash, 16));
        const uint32_t end = static_cast<uint32_t>(std::strtoul(dash + 1, nullptr, 16));
        at = comma + 1;
        if (end <= start || !Memory::Contains(start, end - start)) continue;
        char path[512];
        std::snprintf(path, sizeof path, "%s_%08X.bin", prefix.c_str(), start);
        if (FILE* out = std::fopen(path, "wb")) {
            std::fwrite(Memory::GetPointer(start, end - start), 1, end - start, out);
            std::fclose(out);
            std::fprintf(stderr, "[padscript] dumped 0x%08X-0x%08X to %s\n", start, end, path);
        }
    }
}

// BRAWL_QUICK_MATCH: a stress-test match whose fighters and stage the script does not have to pick. The script still
// walks the menus into a VS match with a CPU in it (any fighters, any stage). "STAGE:CHAR,CHAR[,CHAR,CHAR][:TYPE:RANK]"
// (numbers, 0x for hex; srStageKind and gmCharacterKind as in BrawlHeaders' gm/gm_lib.h: stage 0x01 Battlefield,
// 0x03 Delfino Plaza; character 0x00 Mario, 0x0C Bowser, 0x10 Ice Climbers) makes those 2-4 fighters, all CPUs, on
// that stage. "log" only logs the setups. Two writes, because the game builds the match's setup from the menus'
// selections in the frame the match scene starts (a setup written during the stage select is lost):
//   - during the stage select, every frame: the character selections (so the fighters chosen are the ones loaded);
//   - on the match scene's first frame, after that build and before the stage and fighters load: the stage, the
//     player count and the fighters' state (the build marks slots the character select had not joined as none).
// Layout (BrawlHeaders gm/gm_global.h, gm_sel_char_data.h, gm_global_mode_melee.h): g_GameGlobal (0x805A00E0):
// m_modeMelee +0x08, m_selCharData +0x10. gmSelCharData: gmPlayerInitData[7] at +0xB8, 0x5C bytes each (character
// +0x00, state +0x01). gmGlobalModeMelee: gmMeleeInitData at +0x08 (stage kind u16 at +0x12; player count in byte 1,
// bits 4-2), gmPlayerInitData[7] at +0x98 (state 0 human, 1 CPU, 3 none).
struct QuickMatchSpec {
    bool valid = false;
    bool logOnly = false;
    uint16_t stage = 0;
    std::vector<uint8_t> chars;
    // gmPlayerInitData +0x1E, +0x1F, optional ":TYPE:RANK". Type 0 fights; the vanilla scripts' CPU comes with type 1,
    // which stands still. Rank 100 (level 9) fights hardest; the character select's default is 21 (level 3).
    int cpuType = 0, cpuRank = 100;
};

QuickMatchSpec ParseQuickMatch(const char* text) {
    QuickMatchSpec spec;
    if (text == nullptr || *text == 0) return spec;
    if (std::strcmp(text, "log") == 0) {
        spec.valid = spec.logOnly = true;
        return spec;
    }
    char* end = nullptr;
    spec.stage = static_cast<uint16_t>(std::strtoul(text, &end, 0));
    if (*end != ':') return spec;
    const char* at = end + 1;
    for (;;) {
        const unsigned long kind = std::strtoul(at, &end, 0);
        if (end == at) break;
        spec.chars.push_back(static_cast<uint8_t>(kind));
        if (*end != ',') break;
        at = end + 1;
    }
    if (*end == ':') {
        spec.cpuType = static_cast<int>(std::strtoul(end + 1, &end, 0));
        if (*end == ':') spec.cpuRank = static_cast<int>(std::strtoul(end + 1, &end, 0));
    }
    spec.valid = spec.chars.size() >= 2 && spec.chars.size() <= 4;
    return spec;
}

constexpr uint32_t kGameGlobalSlot = 0x805A00E0u;
constexpr uint32_t kPlayerSize = 0x5Cu;

// g_GameGlobal's pointer at OFFSET, if it points at SIZE bytes of guest memory; else 0.
uint32_t GameGlobalPart(uint32_t offset, uint32_t size) {
    uint32_t global = 0, part = 0;
    if (!Memory::TryRead32(kGameGlobalSlot, global) || !Memory::Contains(global, 0x50)) return 0;
    if (!Memory::TryRead32(global + offset, part) || !Memory::Contains(part, size)) return 0;
    return part;
}

// WHAT's stage kind (unless STAGEAT is 0) and its first four player slots, whole.
void LogSetup(const char* what, uint32_t stageAt, uint32_t players, const std::string& scene, uint64_t frame) {
    char line[4096];
    int n = std::snprintf(line, sizeof line, "[quickmatch] frame %llu %s: %s", static_cast<unsigned long long>(frame),
                          scene.c_str(), what);
    if (stageAt != 0) n += std::snprintf(line + n, sizeof line - n, " stage 0x%02X", Memory::Read16(stageAt));
    for (uint32_t s = 0; s < 4; ++s) {
        n += std::snprintf(line + n, sizeof line - n, " |%u:", s);
        for (uint32_t i = 0; i < kPlayerSize; ++i)
            n += std::snprintf(line + n, sizeof line - n, "%s%02X", i % 4 == 0 ? " " : "", Memory::Read8(players + s * kPlayerSize + i));
    }
    std::fprintf(stderr, "%s\n", line);
    std::fflush(stderr);
}

// The character select's CPU fighter is the template for every fighter (a valid CPU entry: ratios, scale, name); each
// slot keeps its own player id, port and start point, and gets its own costume and team. Slots past the
// fighters get slot 6's (never used by VS) character and state: none.
void ApplyQuickMatch(const QuickMatchSpec& spec, uint32_t selChar) {
    const uint32_t players = selChar + 0xB8u;
    static uint8_t tmpl[kPlayerSize], noneChar = 0, noneState = 0;
    static int captured = 0;  // 1 captured, -1 no CPU found
    if (captured == 0) {  // before the first rewrite, from the character select's own selections
        const uint8_t humanState = Memory::Read8(players + 1);
        int cpu = -1;
        for (uint32_t s = 1; s < 4 && cpu < 0; ++s) {
            const uint32_t p = players + s * kPlayerSize;
            if (Memory::Read8(p) < 0x3E && Memory::Read8(p + 1) != humanState) cpu = static_cast<int>(s);
        }
        if (cpu < 0) {
            captured = -1;
            std::fprintf(stderr, "[quickmatch] no CPU in the character select's selections: nothing rewritten\n");
            return;
        }
        std::memcpy(tmpl, Memory::GetPointer(players + cpu * kPlayerSize, kPlayerSize), kPlayerSize);
        noneChar = Memory::Read8(players + 6 * kPlayerSize);
        noneState = Memory::Read8(players + 6 * kPlayerSize + 1);
        captured = 1;
        std::fprintf(stderr, "[quickmatch] template: slot %d (state %02X; human %02X, none %02X/%02X)\n", cpu, tmpl[1],
                     humanState, noneChar, noneState);
    }
    if (captured < 0) return;
    for (uint32_t s = 0; s < 4; ++s) {
        const uint32_t p = players + s * kPlayerSize;
        if (s >= spec.chars.size()) {
            Memory::Write8(p, noneChar);
            Memory::Write8(p + 1, noneState);
            continue;
        }
        uint8_t slot[kPlayerSize];
        std::memcpy(slot, tmpl, kPlayerSize);
        for (uint32_t f : {0x02u, 0x03u, 0x08u}) slot[f] = Memory::Read8(p + f);  // player id, port, start point
        slot[0x00] = spec.chars[s];
        slot[0x05] = slot[0x06] = static_cast<uint8_t>(s);  // costume
        slot[0x0B] = static_cast<uint8_t>(s);               // team
        for (uint32_t i = 0; i < kPlayerSize; ++i) Memory::Write8(p + i, slot[i]);
    }
}

// The match's own setup, on the match scene's first frame (after the game built it from the selections): the stage,
// the player count (gmMeleeInitData byte 1, bits 4-2) and the fighters' character and state (the game marks slots the
// character select had not joined as none, whatever their selections say).
void ApplyMatchSetup(const QuickMatchSpec& spec, uint32_t mode) {
    const uint32_t init = mode + 0x08u, players = mode + 0x98u;
    Memory::Write16(init + 0x12u, spec.stage);
    const uint8_t count = static_cast<uint8_t>(spec.chars.size());
    Memory::Write8(init + 1u, static_cast<uint8_t>((Memory::Read8(init + 1u) & ~0x1Cu) | (count << 2)));
    for (uint32_t s = 0; s < 4; ++s) {
        const uint32_t p = players + s * kPlayerSize;
        if (s < count) {
            Memory::Write8(p, spec.chars[s]);
            Memory::Write8(p + 1u, 0x01u);  // CPU
            Memory::Write8(p + 0x1Eu, static_cast<uint8_t>(spec.cpuType));
            Memory::Write8(p + 0x1Fu, static_cast<uint8_t>(spec.cpuRank));
        } else {
            Memory::Write8(p + 1u, 0x03u);  // none
        }
    }
}

void QuickMatchFrame(const std::string& scene, uint64_t frame) {
    static const QuickMatchSpec spec = ParseQuickMatch(std::getenv("BRAWL_QUICK_MATCH"));
    if (!spec.valid) return;
    const uint32_t selChar = GameGlobalPart(0x10u, 0x340u), mode = GameGlobalPart(0x08u, 0x320u);
    if (selChar == 0 || mode == 0) return;
    static bool selLogged = false;
    static int meleeFrames = 0;
    if (scene == "scSelStage") {
        if (!selLogged) {  // the character select's selections, as the stage select starts
            LogSetup("selections", 0, selChar + 0xB8u, scene, frame);
            selLogged = true;
        }
        if (!spec.logOnly) ApplyQuickMatch(spec, selChar);
    } else if (scene == "scMelee" && meleeFrames < 2) {  // what the match was set up with
        ++meleeFrames;
        LogSetup("match", mode + 0x08u + 0x12u, mode + 0x98u, scene, frame);
        if (!spec.logOnly && meleeFrames == 1) {
            ApplyMatchSetup(spec, mode);
            LogSetup("match rewritten", mode + 0x08u + 0x12u, mode + 0x98u, scene, frame);
        }
    }
}

// Port 1's scripted input for this frame, if the script has any.
void ApplyPadScript(PADStatus& port1) {
    static const char* const text = std::getenv("BRAWL_PAD_SCRIPT");
    if (text == nullptr) return;
    static const std::vector<PadScriptPress> presses = ParsePadScript(text);
    // The script's clock: frames the game could take input in. While the runtime's start card is up (boot
    // shaders still compiling) game input is blocked, so a press timed into that window would be lost; the clock
    // stands still until it goes, and a scene that came up under it is timed from when it went.
    static uint64_t frame = 0;
    const bool cardUp = settings_overlay::StartupScreenVisible();
    if (cardUp) {
        static uint64_t heldCalls = 0;
        if (++heldCalls % 60 == 0) {
            std::fprintf(stderr, "[padscript] start card holds the clock: %s\n", settings_overlay::StartupScreenReason());
            std::fflush(stderr);
        }
    }
    const uint64_t now = cardUp ? frame : frame++;
    // Each scene change with its frame, so a script can be written from the log alone.
    static std::string lastScene;
    static std::vector<std::pair<std::string, uint64_t>> sceneStarts;  // each scene's first frame
    if (std::string scene = SaveStates::SceneName(); scene != lastScene) {
        // stderr: on Horizon only stderr reaches the debug channel.
        std::fprintf(stderr, "[padscript] frame %llu: scene %s\n", static_cast<unsigned long long>(now), scene.c_str());
        std::fflush(stderr);
        if (std::none_of(sceneStarts.begin(), sceneStarts.end(), [&](const auto& s) { return s.first == scene; }))
            sceneStarts.emplace_back(scene, now);
        // The runtime's start card waits for a real button, which a script never presses. It also covers boot work
        // (the mods' SD card, the code handler's first pass), so a script accepts it at the first screen after boot,
        // as a player would: accepting it on the first frame let the game run hooked code before the hook bodies
        // were written. With P+'s BOOST.GCT running, that first screen is an unlock notice (scPrizeInfo), not the
        // title, and it needs a press the card would block.
        if (!scene.empty() && scene != "scStrap" && scene != "scBoot" && scene != "scMemoryChange")
            settings_overlay::NotifyStrapInputAccepted();
        // BRAWL_DUMP_ON_SCENE="scene:START-END,START-END:prefix": guest memory ranges (hex) written to
        // prefix_START.bin when that scene first comes up (a mod's code as it stands then, for bake_patches.py).
        if (const char* dump = std::getenv("BRAWL_DUMP_ON_SCENE")) {
            const std::string spec(dump);
            const size_t first = spec.find(':');
            if (first != std::string::npos && spec.substr(0, first) == scene) DumpGuestRanges(spec.substr(first + 1));
        }
        lastScene = std::move(scene);
    }
    QuickMatchFrame(lastScene, now);
    if (cardUp) return;  // nothing reaches the game yet
    for (const PadScriptPress& press : presses) {
        if (press.every != 0) {
            if (lastScene != press.scene) continue;
            const auto start = std::find_if(sceneStarts.begin(), sceneStarts.end(),
                                            [&](const auto& s) { return s.first == press.scene; });
            const uint64_t phase = (now - start->second) % press.every;
            if (phase == 0) {
                std::fprintf(stderr, "[padscript] frame %llu: %s\n", static_cast<unsigned long long>(now),
                             press.text.c_str());
                std::fflush(stderr);
            }
            if (phase < press.hold) {
                port1.err = PAD_ERR_NONE;
                port1.button |= press.buttons;
                if (press.stickX != 0) port1.stickX = press.stickX;
                if (press.stickY != 0) port1.stickY = press.stickY;
            }
            continue;
        }
        uint64_t at = press.frame;
        if (!press.scene.empty()) {
            const auto start = std::find_if(sceneStarts.begin(), sceneStarts.end(),
                                            [&](const auto& s) { return s.first == press.scene; });
            if (start == sceneStarts.end()) continue;  // that scene has not come up yet
            at += start->second;
        }
        if (now < at || now >= at + press.hold) continue;
        if (now == at) {
            // With the display copies made so far (gx_copy.cpp): rendered frames against the script's input frames.
            std::fprintf(stderr, "[padscript] frame %llu: %s (display copy %d)\n", static_cast<unsigned long long>(now),
                         press.text.c_str(), g_gxFrameCount);
            std::fflush(stderr);
            if (press.shot || press.dump) {
                std::string item = press.text;
                std::replace(item.begin(), item.end(), ':', '-');
                std::replace(item.begin(), item.end(), '*', '-');
                if (press.shot) aurora_request_frame_capture(0, ("shot_" + item + ".bmp").c_str());
                if (press.dump) aurora_request_draw_dump((item + ".txt").c_str());
            }
            if (press.memdump) {
                if (const char* ranges = std::getenv("BRAWL_DUMP_RANGES")) DumpGuestRanges(ranges);
            }
            if (press.exit) {
#if defined(__SWITCH__)
                HorizonExitNow();
#else
                // A profile-instrumented build (-fprofile-instr-generate) writes its profile from an atexit
                // handler, which _Exit skips: write it here. Weak, so other builds link without the profile runtime.
                if (__llvm_profile_write_file != nullptr) __llvm_profile_write_file();
                std::_Exit(0);
#endif
            }
        }
        port1.err = PAD_ERR_NONE;
        port1.button |= press.buttons;
        if (press.stickX != 0) port1.stickX = press.stickX;
        if (press.stickY != 0) port1.stickY = press.stickY;
    }
}
} // namespace

// PADRead: gathers every GameCube pad source for the frame and writes the statuses to guest memory. In network
// play every port is the frame's latched input from all players instead (frame_input.h).
extern "C" uint32_t PAD__Read_HLE(uint32_t statusPtr)
{
    if (statusPtr == 0) {
        return 0;
    }

    PADStatus statuses[PAD_CHANMAX]{};
    uint32_t rumbleMask = 0;
    if (FrameInput::Active()) {
        for (uint32_t port = 0; port < PAD_CHANMAX; ++port) {
            const FrameInput::GcPad& pad = FrameInput::Get(port).pad;
            PADStatus& s = statuses[port];
            s.err = pad.connected ? PAD_ERR_NONE : PAD_ERR_NO_CONTROLLER;
            if (!pad.connected) continue;
            s.button = pad.button;
            s.stickX = pad.stickX;
            s.stickY = pad.stickY;
            s.substickX = pad.substickX;
            s.substickY = pad.substickY;
            s.triggerLeft = pad.triggerL;
            s.triggerRight = pad.triggerR;
            rumbleMask |= 0x80000000u >> port;
        }
    } else {
        rumbleMask = ReadLocalPads(statuses);
        ApplyPadScript(statuses[0]);
    }
    // P+'s codes in the SDK's SI path, which this PADRead replaces (pplus_si_codes.h).
    PplusSiCodes::Apply(statuses);

    // Which ports report a controller, and the device behind each: logged whenever that changes.
    static std::string lastPorts;
    std::string ports;
    for (uint32_t port = 0; port < PAD_CHANMAX; ++port) {
        if (statuses[port].err != PAD_ERR_NONE) continue;
        const s32 index = PADGetIndexForPort(port);
        SDL_Gamepad* gamepad = index >= 0 ? PADGetSDLGamepadForIndex(static_cast<u32>(index)) : nullptr;
        const char* name = gamepad != nullptr ? SDL_GetGamepadName(gamepad) : nullptr;
        ports += " P" + std::to_string(port + 1) + "=" + (name != nullptr ? name : index >= 0 ? "pad" : "keyboard/script");
    }
    if (ports != lastPorts) {
        lastPorts = ports;
        std::printf("[pad] controllers:%s\n", ports.empty() ? " none" : ports.c_str());
        std::fflush(stdout);
    }

    try {
        for (uint32_t i = 0; i < PAD_CHANMAX; ++i) {
            WritePadStatus(statusPtr + static_cast<uint32_t>(i * PadStatusContract::kGuestStatusSize),
                           statuses[i]);
        }
    } catch (const Memory::AccessViolation&) {
        return 0;
    }

    return rumbleMask;
}
// Brawl (RSBE01): GameCube controllers are its main input.
PPC_NATIVE_OVERRIDE(80215FC4, PAD__Read_HLE, uint32_t, (uint32_t statusPtr), (statusPtr));

extern "C" uint32_t PAD__Reset_HLE(uint32_t mask)
{
    return PADReset(mask) ? 1u : 0u;
}
// Brawl (RSBE01): GameCube controllers are its main input.
PPC_NATIVE_OVERRIDE(80215C54, PAD__Reset_HLE, uint32_t, (uint32_t mask), (mask));

extern "C" uint32_t PAD__Recalibrate_HLE(uint32_t mask)
{
    return PADRecalibrate(mask) ? 1u : 0u;
}
// Brawl (RSBE01): GameCube controllers are its main input.
PPC_NATIVE_OVERRIDE(80215D5C, PAD__Recalibrate_HLE, uint32_t, (uint32_t mask), (mask));

extern "C" void PAD__ControlMotor_HLE(int32_t chan, uint32_t command)
{
    if (command == PAD_MOTOR_RUMBLE && !g_rumbleEnabled.load(std::memory_order_relaxed)) {
        command = PAD_MOTOR_STOP;
    }
    if (BrawlOnlineCore::DolphinControlsRumble(chan, command == PAD_MOTOR_RUMBLE)) return;
    PADControlMotor(chan, command);
}
// Brawl (RSBE01): GameCube controllers are its main input.
PPC_NATIVE_OVERRIDE_VOID(802162A4, PAD__ControlMotor_HLE, (int32_t chan, uint32_t command), (chan, command));
