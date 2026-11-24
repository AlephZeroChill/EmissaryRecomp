// Brawl Online's in-game UI in EmissaryRecomp (brawl_online_bridge.h): brawlonline.net core/ui, the
// same code Dolphin draws its on-screen messages, chat window and replay overlay with, drawn here in
// the runtime's ImGui frame (aurora's ImGui, Dolphin's version) with Dolphin's fonts and style.
// The chat key is Dolphin's setting ([Online] ChatKey in Dolphin.ini, a Dolphin input expression:
// keyboard keys by their Windows names, controller buttons by Dolphin's), "T" when unset.

#include "brawl_online_bridge.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <string_view>
#include <unordered_map>

#include <imgui.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>

#include <dolphin/pad.h>

#include "BrawlOnline/Config/OnlineSettings.h"
#include "BrawlOnline/Rollback/GameplaySession.h"
#include "BrawlOnline/UI/Hooks.h"
#include "BrawlOnline/UI/InputDisplayUI.h"
#include "BrawlOnline/UI/OnScreenDisplay.h"
#include "BrawlOnline/UI/OnlineChatUI.h"
#include "BrawlOnline/UI/ReplayOverlayUI.h"
#include "BrawlOnline/UI/Style.h"
#include "Common/CommonPaths.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "input_expr.h"

namespace settings_overlay {
void NotifyStrapInputAccepted() noexcept;  // runtime/include/settings_overlay.h (the runtime's headers stay out)
}

namespace B = BrawlOnlineBridge;
namespace UI = BrawlOnline::UI;
namespace ChatUI = VideoCommon::OnlineChatUI;

namespace {

// Dolphin's settings the UI follows (Core/Config/MainSettings.cpp: the same keys and defaults).
const Config::Info<bool> OSD_MESSAGES{{Config::System::Main, "Interface", "OnScreenDisplayMessages"}, true};
const Config::Info<int> OSD_FONT_SIZE{{Config::System::Main, "Settings", "OSDFontSize"}, UI::DEFAULT_FONT_SIZE};

bool s_drawn = false;
UI::Fonts s_fonts;

// ---- The chat key ---------------------------------------------------------------------------------
std::string s_key_text;
InputExpr::Expression s_key;
bool s_key_was_down = false;

// Dolphin's keyboard names on Windows (DirectInput's, InputCommon DInput/NamedKeys.h) that SDL names
// differently; letters, digits and F1-F15 are the same in both.
const std::unordered_map<std::string_view, SDL_Scancode>& KeyNames() {
    static const std::unordered_map<std::string_view, SDL_Scancode> names = {
        {"UP", SDL_SCANCODE_UP}, {"DOWN", SDL_SCANCODE_DOWN}, {"LEFT", SDL_SCANCODE_LEFT},
        {"RIGHT", SDL_SCANCODE_RIGHT}, {"ADD", SDL_SCANCODE_KP_PLUS}, {"APOSTROPHE", SDL_SCANCODE_APOSTROPHE},
        {"APPS", SDL_SCANCODE_APPLICATION}, {"BACK", SDL_SCANCODE_BACKSPACE}, {"BACKSLASH", SDL_SCANCODE_BACKSLASH},
        {"CAPITAL", SDL_SCANCODE_CAPSLOCK}, {"COMMA", SDL_SCANCODE_COMMA}, {"DECIMAL", SDL_SCANCODE_KP_PERIOD},
        {"DELETE", SDL_SCANCODE_DELETE}, {"DIVIDE", SDL_SCANCODE_KP_DIVIDE}, {"EQUALS", SDL_SCANCODE_EQUALS},
        {"ESCAPE", SDL_SCANCODE_ESCAPE}, {"GRAVE", SDL_SCANCODE_GRAVE}, {"HOME", SDL_SCANCODE_HOME},
        {"END", SDL_SCANCODE_END}, {"INSERT", SDL_SCANCODE_INSERT}, {"MINUS", SDL_SCANCODE_MINUS},
        {"MULTIPLY", SDL_SCANCODE_KP_MULTIPLY}, {"NUMLOCK", SDL_SCANCODE_NUMLOCKCLEAR},
        {"NUMPAD0", SDL_SCANCODE_KP_0}, {"NUMPAD1", SDL_SCANCODE_KP_1}, {"NUMPAD2", SDL_SCANCODE_KP_2},
        {"NUMPAD3", SDL_SCANCODE_KP_3}, {"NUMPAD4", SDL_SCANCODE_KP_4}, {"NUMPAD5", SDL_SCANCODE_KP_5},
        {"NUMPAD6", SDL_SCANCODE_KP_6}, {"NUMPAD7", SDL_SCANCODE_KP_7}, {"NUMPAD8", SDL_SCANCODE_KP_8},
        {"NUMPAD9", SDL_SCANCODE_KP_9}, {"NUMPADENTER", SDL_SCANCODE_KP_ENTER},
        {"NUMPADEQUALS", SDL_SCANCODE_KP_EQUALS}, {"NUMPADCOMMA", SDL_SCANCODE_KP_COMMA},
        {"OEM_102", SDL_SCANCODE_NONUSBACKSLASH}, {"PAUSE", SDL_SCANCODE_PAUSE}, {"PERIOD", SDL_SCANCODE_PERIOD},
        {"PRIOR", SDL_SCANCODE_PAGEUP}, {"NEXT", SDL_SCANCODE_PAGEDOWN}, {"RETURN", SDL_SCANCODE_RETURN},
        {"LBRACKET", SDL_SCANCODE_LEFTBRACKET}, {"RBRACKET", SDL_SCANCODE_RIGHTBRACKET},
        {"LCONTROL", SDL_SCANCODE_LCTRL}, {"RCONTROL", SDL_SCANCODE_RCTRL}, {"LMENU", SDL_SCANCODE_LALT},
        {"RMENU", SDL_SCANCODE_RALT}, {"LSHIFT", SDL_SCANCODE_LSHIFT}, {"RSHIFT", SDL_SCANCODE_RSHIFT},
        {"LWIN", SDL_SCANCODE_LGUI}, {"RWIN", SDL_SCANCODE_RGUI}, {"SCROLL", SDL_SCANCODE_SCROLLLOCK},
        {"SEMICOLON", SDL_SCANCODE_SEMICOLON}, {"SLASH", SDL_SCANCODE_SLASH}, {"SPACE", SDL_SCANCODE_SPACE},
        {"SUBTRACT", SDL_SCANCODE_KP_MINUS}, {"SYSRQ", SDL_SCANCODE_PRINTSCREEN}, {"TAB", SDL_SCANCODE_TAB},
    };
    return names;
}

// Dolphin's controller button names (XInput's and SDL's) on SDL's buttons, as the runtime's own
// controller expressions read them (input_bindings.cpp).
const std::unordered_map<std::string_view, SDL_GamepadButton>& ButtonNames() {
    static const std::unordered_map<std::string_view, SDL_GamepadButton> names = {
        {"Button A", SDL_GAMEPAD_BUTTON_SOUTH}, {"Button B", SDL_GAMEPAD_BUTTON_EAST},
        {"Button X", SDL_GAMEPAD_BUTTON_WEST}, {"Button Y", SDL_GAMEPAD_BUTTON_NORTH},
        {"Button S", SDL_GAMEPAD_BUTTON_SOUTH}, {"Button E", SDL_GAMEPAD_BUTTON_EAST},
        {"Button W", SDL_GAMEPAD_BUTTON_WEST}, {"Button N", SDL_GAMEPAD_BUTTON_NORTH},
        {"Shoulder L", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER}, {"Shoulder R", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
        {"Thumb L", SDL_GAMEPAD_BUTTON_LEFT_STICK}, {"Thumb R", SDL_GAMEPAD_BUTTON_RIGHT_STICK},
        {"Start", SDL_GAMEPAD_BUTTON_START}, {"Back", SDL_GAMEPAD_BUTTON_BACK}, {"Guide", SDL_GAMEPAD_BUTTON_GUIDE},
        {"Pad N", SDL_GAMEPAD_BUTTON_DPAD_UP}, {"Pad S", SDL_GAMEPAD_BUTTON_DPAD_DOWN},
        {"Pad W", SDL_GAMEPAD_BUTTON_DPAD_LEFT}, {"Pad E", SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
    };
    return names;
}

// One input of the expression: `DInput/0/Keyboard Mouse:T`, `T`, `XInput/0/Gamepad:Button A`...
double ReadInput(const std::string& qualified) {
    const std::string_view name = std::string_view(qualified).substr(qualified.rfind(':') + 1);
    if (const auto b = ButtonNames().find(name); b != ButtonNames().end()) {
        for (u32 index = 0; index < 4; ++index)
            if (SDL_Gamepad* pad = PADGetSDLGamepadForIndex(index); pad && SDL_GetGamepadButton(pad, b->second))
                return 1.0;
        return 0.0;
    }
    SDL_Scancode code = SDL_SCANCODE_UNKNOWN;
    if (const auto k = KeyNames().find(name); k != KeyNames().end())
        code = k->second;
    else
        code = SDL_GetScancodeFromName(std::string(name).c_str());  // A-Z, 0-9, F1-F15, SDL's names
    int count = 0;
    const bool* keys = SDL_GetKeyboardState(&count);
    return code != SDL_SCANCODE_UNKNOWN && code < count && keys[code] && SDL_GetKeyboardFocus() ? 1.0 : 0.0;
}

// The "Activate NetPlay Chat" binding, as Dolphin's HotkeyManager: the launcher's [Online] ChatKey,
// else Dolphin's default T. A press opens the box (never while it has the keyboard: then it is text).
void PollChatKey() {
    std::string text = Config::Get(Config::ONLINE_CHAT_KEY);
    if (text.empty()) text = "T";
    if (text != s_key_text) {
        s_key_text = text;
        std::string error;
        if (!InputExpr::Expression::Parse(text, s_key, error)) {
            WARN_LOG_FMT(BRAWLBACK, "chat key {}: {}", text, error);
            s_key = InputExpr::Expression();
        }
        ChatUI::SetChatKey(text);
        s_key_was_down = true;  // not a press until it is let go
    }
    if (s_key.Empty()) return;
    const bool down = s_key.Evaluate(ReadInput) > InputExpr::kConditionThreshold;
    if (down && !s_key_was_down && !ChatUI::IsTyping()) ChatUI::Activate();
    s_key_was_down = down;
}

void Start() {
    // Dolphin's: the OSD font a player put in User/Load, else the one in Sys/Resources (copied
    // beside the executable).
    const std::string resources = File::GetSysDirectory() + RESOURCES_DIR DIR_SEP;
    s_fonts = UI::AddFonts(ImGui::GetIO().Fonts, File::GetUserPath(D_LOAD_IDX) + "OSD_Font.ttf", resources, false);
    ChatUI::SetFont(s_fonts.chat);
    VideoCommon::ReplayOverlayUI::SetFont(s_fonts.chat);
    ChatUI::InstallClipboard();
    INFO_LOG_FMT(BRAWLBACK, "UI fonts from {}: OSD {}, chat {}", resources, s_fonts.osd != nullptr,
                 s_fonts.chat != nullptr);
}

}  // namespace

namespace BrawlOnlineBridge {

// At the core's start (before any message): messages are logged as before, and drawn.
void CoreUiStart() {
    UI::Hooks hooks;
    hooks.show_messages = [] { return Config::Get(OSD_MESSAGES); };
    hooks.message_added = [](std::string_view text, u32 ms, u32 argb) {
        const std::string t(text);
        INFO_LOG_FMT(BRAWLBACK, "[screen] {}", t);
        B::ScreenMessage(t.c_str(), ms, argb);
    };
    UI::SetHooks(std::move(hooks));
}

void CoreUiDraw(float top) {
    if (!s_drawn) {
        s_drawn = true;
        Start();
    }
    PollChatKey();
    // A replay or a watched game (the launcher's playback, BRAWL_ONLINE_REPLAY_COMM): nobody presses
    // A on the title card, which would cover it the whole time.
    if (Gprb::Session::GetPlaybackInfo().active) settings_overlay::NotifyStrapInputAccepted();

    // Dolphin's ImGui coordinates are the backbuffer's pixels, its scale the screen's DPI scale; the
    // runtime's are the window's points, its framebuffer scale applied when drawn. So: scale 1, and
    // no framebuffer scale while this draws (the OSD's margins read it). The runtime's own style
    // and font come back after.
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 framebuffer_scale = io.DisplayFramebufferScale;
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
    const ImGuiStyle runtime_style = ImGui::GetStyle();
    ImGuiStyle& style = ImGui::GetStyle();
    UI::SetStyle(style, 1.0f);
    style.FontSizeBase = static_cast<float>(std::clamp(Config::Get(OSD_FONT_SIZE), 6, 64));
    ImGui::PushFont(s_fonts.osd, style.FontSizeBase);

    // The runtime's FPS box is in the top-right corner: the top-left one is free.
    ChatUI::SetTopLeftBoxRight(0.0f);
    UI::InputDisplay::Display(1.0f);  // under the windows (the background draw list)
    ChatUI::Display(1.0f);
    VideoCommon::ReplayOverlayUI::Display(1.0f);  // while a replay or a spectated game plays
    OSD::DrawMessages(std::max(top, ChatUI::CornerBarBottom()));

    ImGui::PopFont();
    style = runtime_style;
    io.DisplayFramebufferScale = framebuffer_scale;
}

bool CoreUiBlocksInput() { return ChatUI::IsTyping(); }

}  // namespace BrawlOnlineBridge
