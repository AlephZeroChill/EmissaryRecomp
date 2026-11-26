#pragma once

// Brawl-styled menu drawing shared by the online menu and the options menu: the game's own fonts (read from the disc)
// in its 456-unit-tall layout space, slanted plates for buttons and banners, the Smash Ball, and one input reader for
// every controller, the keyboard and the mouse.

#include "netplay/game_layout.h"

#include <aurora/imgui.h>
#include <dolphin/pad.h>
#include <imgui.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keyboard.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>

extern "C" uint8_t* aurora_load_png_rgba(const char* path, uint32_t* width, uint32_t* height);

namespace MenuKit {
namespace GL = GameLayout;

constexpr uint32_t kPlayer[4] = {0xE0382CFFu, 0x2F6FDCFFu, 0xE8B421FFu, 0x2FA84AFFu};
constexpr uint32_t kOrange = 0xFF8C28FFu, kGold = 0xFFD966FFu, kSnow = 0xF4F4F8FFu, kDim = 0xA8ACBEFFu,
                   kInk = 0x0B0D16FFu, kRed = 0xFF6A52FFu, kGreen = 0x6FDB6FFFu;

inline ImU32 Col(uint32_t rgba, float alpha = 1.0f) {
    return IM_COL32(rgba >> 24, (rgba >> 16) & 255, (rgba >> 8) & 255, static_cast<int>((rgba & 255) * alpha));
}
inline uint32_t Shade(uint32_t rgba, float f) {
    auto c = [&](int shift) { return static_cast<uint32_t>(std::clamp(((rgba >> shift) & 255) * f, 0.0f, 255.0f)); };
    return (c(24) << 24) | (c(16) << 16) | (c(8) << 8) | (rgba & 255);
}


// ---- input: edges of directions and buttons from every controller, the keyboard and the mouse ----
struct Nav {
    bool up = false, down = false, left = false, right = false, confirm = false, back = false;
    bool start = false;                     // a controller's Start went down (it is in `confirm` too)
    bool prevTab = false, nextTab = false;  // a controller's L / R (shoulder or trigger) went down
    bool startHeld = false;                 // Start is held on a controller
};

// The menus' last input came from a controller (true) or from the keyboard and mouse (false): which button prompts
// they show.
inline bool& LastInputController() {
    static bool controller = false;
    return controller;
}

// The buttons that are the game's A and B on a controller, so the menus confirm and go back with the same buttons the
// game does: a GameCube controller's B is SDL's west button (east is its X), a Wii U Pro Controller's A is east and its
// B south. A controller on a port uses that port's mapping (a player's remap too); one on no port, its kind's default.
struct FaceButtons {
    SDL_GamepadButton a = SDL_GAMEPAD_BUTTON_SOUTH, b = SDL_GAMEPAD_BUTTON_EAST;
};

inline FaceButtons FaceButtonsFor(u32 index, SDL_Gamepad* pad) {
    FaceButtons face;
    const SDL_GamepadType type = SDL_GetGamepadType(pad);
    const char* name = SDL_GetGamepadName(pad);
    if (type == SDL_GAMEPAD_TYPE_GAMECUBE ||
        (type == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO && SDL_GetGamepadProduct(pad) == 0x2073)) {
        face.b = SDL_GAMEPAD_BUTTON_WEST;
    } else if (name != nullptr && std::strstr(name, "Wii U Pro Controller") != nullptr) {
        face.a = SDL_GAMEPAD_BUTTON_EAST;
        face.b = SDL_GAMEPAD_BUTTON_SOUTH;
    }
    for (u32 port = 0; port < PAD_MAX_CONTROLLERS; ++port) {
        if (PADGetIndexForPort(port) != static_cast<s32>(index)) continue;
        u32 count = 0;
        const PADButtonMapping* mapping = PADGetButtonMappings(port, &count);
        for (u32 j = 0; mapping != nullptr && j < count; ++j) {
            if (mapping[j].nativeButton >= SDL_GAMEPAD_BUTTON_COUNT) continue;
            const auto native = static_cast<SDL_GamepadButton>(mapping[j].nativeButton);
            if (mapping[j].padButton == PAD_BUTTON_A) face.a = native;
            if (mapping[j].padButton == PAD_BUTTON_B) face.b = native;
        }
        break;
    }
    return face;
}

inline Nav ReadNav(bool typing = false) {
    static uint32_t prevHeld = 0;
    static double nextRepeat = 0.0, repeatStart = 0.0;
    enum : uint32_t { kUp = 1, kDown = 2, kLeft = 4, kRight = 8, kA = 16, kB = 32, kStart = 64, kL = 128, kR = 256 };
    uint32_t held = 0;
    // Only the game window in front reads controllers (two games on one PC share them).
    for (u32 i = 0; i < 8 && SDL_GetKeyboardFocus() != nullptr; ++i) {
        SDL_Gamepad* pad = PADGetSDLGamepadForIndex(i);
        if (pad == nullptr) continue;
        auto b = [&](SDL_GamepadButton button) { return SDL_GetGamepadButton(pad, button); };
        const int lx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX), ly = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
        if (b(SDL_GAMEPAD_BUTTON_DPAD_UP) || ly < -20000) held |= kUp;
        if (b(SDL_GAMEPAD_BUTTON_DPAD_DOWN) || ly > 20000) held |= kDown;
        if (b(SDL_GAMEPAD_BUTTON_DPAD_LEFT) || lx < -20000) held |= kLeft;
        if (b(SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || lx > 20000) held |= kRight;
        const FaceButtons face = FaceButtonsFor(i, pad);
        if (b(face.a)) held |= kA;
        if (b(face.b)) held |= kB;
        if (b(SDL_GAMEPAD_BUTTON_START)) held |= kStart;
        // L and R: the shoulders, or the triggers pressed past half (a GameCube controller's L and R are triggers, and
        // its Z is SDL's right shoulder).
        if (b(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) || SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000) held |= kL;
        if ((b(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) && SDL_GetGamepadType(pad) != SDL_GAMEPAD_TYPE_GAMECUBE) ||
            SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000)
            held |= kR;
    }
    const double now = ImGui::GetTime();
    uint32_t pressed = held & ~prevHeld;
    // A held direction repeats, as menus do, faster once it has been held a while (a volume from 0 to 100 in about two
    // seconds).
    const uint32_t dirs = held & (kUp | kDown | kLeft | kRight);
    if (dirs != 0 && (dirs & prevHeld) == dirs && now >= nextRepeat) {
        pressed |= dirs;
        nextRepeat = now + (now - repeatStart > 1.0 ? 0.045 : 0.09);
    } else if ((pressed & (kUp | kDown | kLeft | kRight)) != 0) {
        nextRepeat = now + 0.35;
        repeatStart = now;
    }
    if ((held & ~prevHeld) != 0) LastInputController() = true;
    prevHeld = held;
    Nav nav;
    nav.up = (pressed & kUp) || ImGui::IsKeyPressed(ImGuiKey_UpArrow);
    nav.down = (pressed & kDown) || ImGui::IsKeyPressed(ImGuiKey_DownArrow);
    nav.left = (pressed & kLeft) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow);
    nav.right = (pressed & kRight) || ImGui::IsKeyPressed(ImGuiKey_RightArrow);
    nav.start = (pressed & kStart) != 0;
    nav.startHeld = (held & kStart) != 0;
    nav.prevTab = (pressed & kL) != 0;
    nav.nextTab = (pressed & kR) != 0;
    nav.confirm = (pressed & (kA | kStart)) || ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter) ||
                  (!typing && ImGui::IsKeyPressed(ImGuiKey_Space));
    nav.back = (pressed & kB) || ImGui::IsKeyPressed(ImGuiKey_Escape);
    const ImGuiIO& io = ImGui::GetIO();
    if (io.InputQueueCharacters.Size > 0 || io.MouseWheel != 0 || ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
        ImGui::IsKeyPressed(ImGuiKey_UpArrow) || ImGui::IsKeyPressed(ImGuiKey_DownArrow) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow) ||
        ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_Escape) ||
        std::fabs(io.MouseDelta.x) + std::fabs(io.MouseDelta.y) > 2.0f)
        LastInputController() = false;
    return nav;
}

// ---- drawing in layout units (456 tall, y up, 0 at the centre) ----
struct Canvas {
    ImDrawList* list = nullptr;
    GL::View view;
    float t = 0.0f;
    ImVec2 P(float x, float y) const { return ImVec2(view.centerX + x * view.scale, view.centerY - y * view.scale); }
};

// ---- vector fonts, drawn at the screen's own resolution ----
// Brawl Online's build ships TrueType fonts in Sys/Resources beside the program: Barlow Condensed (Brawl Online's
// headline face, brawlonline.net's) for titles, buttons and values, and Noto Sans (Dolphin's too) for sentences and for
// any letter Barlow lacks. The game's own fonts are bitmaps 20 to 32 pixels tall, blurred when a big window draws them
// several times that size. Without the TrueType files the menus draw with the game's fonts as before.
struct VectorFonts {
    ImFont* display = nullptr;  // Barlow Condensed Black Italic
    ImFont* label = nullptr;    // Barlow Condensed SemiBold
    ImFont* body = nullptr;     // Noto Sans
};

inline const VectorFonts& Fonts() {
    static const VectorFonts fonts = [] {
        VectorFonts f;
        const char* base = SDL_GetBasePath();
        if (base == nullptr) return f;
        const std::filesystem::path dir = std::filesystem::u8path(base) / "Sys" / "Resources";
        auto path = [&](const char* file) {
            std::error_code ec;
            const auto p = dir / file;
            if (!std::filesystem::exists(p, ec)) return std::string();
            const auto utf8 = p.u8string();  // ImGui opens files by UTF-8 name
            return std::string(utf8.begin(), utf8.end());
        };
        const std::string noto = path("NotoSans-Regular.ttf"), notoJp = path("NotoSansJP-Regular.ttf");
        ImFontAtlas* atlas = ImGui::GetIO().Fonts;
        auto add = [&](const std::string& file, const char* name) -> ImFont* {
            if (file.empty()) return nullptr;
            ImFontConfig config;
            std::snprintf(config.Name, sizeof config.Name, "%s", name);
            ImFont* font = atlas->AddFontFromFileTTF(file.c_str(), 0.0f, &config);
            if (font == nullptr) return nullptr;
            // Letters the face lacks (in a controller's name, say) come from Noto Sans, then Noto Sans JP.
            for (const std::string* fallback : {&noto, &notoJp}) {
                if (fallback->empty() || *fallback == file) continue;
                ImFontConfig merge;
                merge.MergeMode = true;
                atlas->AddFontFromFileTTF(fallback->c_str(), 0.0f, &merge);
            }
            return font;
        };
        f.display = add(path("BarlowCondensed-BlackItalic.ttf"), "Menu display");
        f.label = add(path("BarlowCondensed-SemiBold.ttf"), "Menu label");
        f.body = add(noto, "Menu body");
        if (f.display == nullptr || f.label == nullptr || f.body == nullptr) f = VectorFonts{};
        return f;
    }();
    return fonts;
}
inline bool HaveVectorFonts() { return Fonts().display != nullptr; }

inline std::shared_ptr<GL::Font> TitleFont() {
    static const auto font = GL::LoadFont("/system/font/font_hira.brfnt");
    return font;
}
inline std::shared_ptr<GL::Font> MenuFont() {
    static const auto font = GL::LoadFont("/system/font/font_melee.brfnt");
    return GL::FontReady(font.get()) ? font : TitleFont();
}

inline ImVec4 ColF(uint32_t rgba, float alpha) { return ImGui::ColorConvertU32ToFloat4(Col(rgba, alpha)); }

// `text` in a vector font, `size` layout units tall, its capitals centred on y: originH 0 starts it at x, 1 centres it
// there, 2 ends it there. Text wider than maxWidth is drawn smaller. Top to bottom it fades from `top` to `bottom`;
// `outline` draws a dark edge around it, as Brawl's titles have. Placed on whole pixels, so it stays sharp. Returns its
// width in layout units.
inline float VText(const Canvas& c, ImFont* font, const std::string& text, float x, float y, float size, uint32_t top,
                   uint32_t bottom, int originH = 1, float maxWidth = 0.0f, bool outline = false, float alpha = 1.0f) {
    if (text.empty() || font == nullptr) return 0.0f;
    float px = std::max(6.0f, std::round(size * c.view.scale));
    float width = font->CalcTextSizeA(px, FLT_MAX, 0.0f, text.c_str()).x;
    if (maxWidth > 0.0f && width > maxWidth * c.view.scale) {
        px = std::max(6.0f, std::floor(px * maxWidth * c.view.scale / width));
        width = font->CalcTextSizeA(px, FLT_MAX, 0.0f, text.c_str()).x;
    }
    const float ascent = font->GetFontBaked(px)->Ascent;
    const ImVec2 at = c.P(x, y);
    // Capitals are about 0.7 of the size tall: their middle goes on y.
    const ImVec2 pos(std::round(at.x - width * 0.5f * originH), std::round(at.y + px * 0.35f - ascent));
    if (outline) {
        const float d = std::max(1.0f, std::round(px * 0.065f));
        for (int j = 0; j < 8; ++j) {
            const float a = j * 0.785398f;
            c.list->AddText(font, px, ImVec2(pos.x + std::round(std::cos(a) * d), pos.y + std::round(std::sin(a) * d)),
                            Col(kInk, alpha), text.c_str());
        }
    }
    const int first = c.list->VtxBuffer.Size;
    c.list->AddText(font, px, pos, Col(top, alpha), text.c_str());
    if (top != bottom) {
        const float y1 = pos.y + ascent, y0 = y1 - px * 0.72f;
        const ImVec4 a = ColF(top, alpha), b = ColF(bottom, alpha);
        for (int i = first; i < c.list->VtxBuffer.Size; ++i) {
            ImDrawVert& v = c.list->VtxBuffer[i];
            const float t = std::clamp((v.pos.y - y0) / std::max(1.0f, y1 - y0), 0.0f, 1.0f);
            v.col = ImGui::ColorConvertFloat4ToU32(ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t,
                                                          a.w + (b.w - a.w) * t));
        }
    }
    return width / c.view.scale;
}

// How wide `text` is in layout units at `size`.
inline float VTextWidth(const Canvas& c, ImFont* font, const std::string& text, float size) {
    if (font == nullptr) return 0.0f;
    const float px = std::max(6.0f, std::round(size * c.view.scale));
    return font->CalcTextSizeA(px, FLT_MAX, 0.0f, text.c_str()).x / c.view.scale;
}

inline void Text(const Canvas& c, const GL::Font& font, const std::string& text, float x, float y, float size, uint32_t top,
          uint32_t bottom, int originH = 1, float maxWidth = 0.0f, bool outline = false) {
    if (HaveVectorFonts()) {
        // In the game's title font's place, Barlow's black italic (a little smaller: its letters are bigger).
        VText(c, &font == TitleFont().get() ? Fonts().display : Fonts().label, text, x, y, size * 0.94f, top, bottom, originH,
              maxWidth, outline);
        return;
    }
    const auto u = GL::Utf16(text);
    if (maxWidth > 0.0f) {
        const float w = GL::MeasureText(font, u, size);
        if (w > maxWidth) size *= maxWidth / w;
    }
    if (outline) {
        for (int j = 0; j < 12; ++j) {
            const float a = j * 0.5236f, d = std::max(1.5f, size * 0.07f);
            GL::DrawText(c.list, c.view, font, u, x + std::cos(a) * d, y + std::sin(a) * d, size, kInk, kInk, 1.0f, originH, 1);
        }
    }
    GL::DrawText(c.list, c.view, font, u, x, y, size, top, bottom, 1.0f, originH, 1);
}

inline void Body(const Canvas& c, const std::string& text, float x, float y, float size, uint32_t color = kSnow, int originH = 1,
          float maxWidth = 0.0f) {
    if (HaveVectorFonts()) {
        VText(c, Fonts().label, text, x, y, size * 1.1f, color, color, originH, maxWidth);
        return;
    }
    Text(c, *MenuFont(), text, x, y, size, color, color, originH, maxWidth);
}

// A small arrow (Brawl's fonts have no arrow glyphs): dir 0 up, 1 down, 2 left, 3 right.
inline void Arrow(const Canvas& c, float x, float y, float size, int dir, uint32_t color) {
    const float h = size * 0.5f;
    ImVec2 a, b, d;
    switch (dir) {
    case 0: a = c.P(x, y + h); b = c.P(x + h, y - h); d = c.P(x - h, y - h); break;
    case 1: a = c.P(x, y - h); b = c.P(x - h, y + h); d = c.P(x + h, y + h); break;
    case 2: a = c.P(x - h, y); b = c.P(x + h, y + h); d = c.P(x + h, y - h); break;
    default: a = c.P(x + h, y); b = c.P(x - h, y - h); d = c.P(x - h, y + h); break;
    }
    c.list->AddTriangleFilled(a, b, d, Col(color));
}

// A slanted plate, the shape of Brawl's menu buttons and banners: a lighter top band over the base colour, outlined.
inline void Plate(const Canvas& c, float x0, float y0, float x1, float y1, uint32_t color, float slant = 10.0f, float alpha = 1.0f) {
    const float ym = (y0 + y1) * 0.5f;
    auto Edge = [&](float y) { return slant * (2.0f * (y - y0) / std::max(1.0f, y1 - y0) - 1.0f); };  // -slant..+slant
    const ImVec2 tl = c.P(x0 + Edge(y1), y1), tr = c.P(x1 + Edge(y1), y1), ml = c.P(x0 + Edge(ym), ym),
                 mr = c.P(x1 + Edge(ym), ym), bl = c.P(x0 + Edge(y0), y0), br = c.P(x1 + Edge(y0), y0);
    c.list->AddQuadFilled(tl, tr, mr, ml, Col(Shade(color, 1.22f), alpha));
    c.list->AddQuadFilled(ml, mr, br, bl, Col(color, alpha));
    c.list->AddLine(ml, mr, Col(Shade(color, 1.45f), alpha * 0.5f), std::max(1.0f, c.view.scale * 0.5f));
    // The edge grows with the window (it was 3 pixels at any size: heavy in a small window, a hairline in 4K).
    c.list->AddQuad(tl, tr, br, bl, Col(kInk, alpha), std::max(1.5f, c.view.scale * 1.4f));
}

// The outline of Plate()'s shape, `grow` layout units outside it: a focused plate's edge.
inline void PlateOutline(const Canvas& c, float x0, float y0, float x1, float y1, float slant, float grow, uint32_t color,
                         float alpha, float thickness) {
    const float g = grow;
    const ImVec2 pts[4] = {c.P(x0 + slant - g, y1 + g), c.P(x1 + slant + g, y1 + g), c.P(x1 - slant + g, y0 - g),
                           c.P(x0 - slant - g, y0 - g)};
    c.list->AddPolyline(pts, 4, Col(color, alpha), ImDrawFlags_Closed, std::max(1.0f, thickness * c.view.scale));
}

// A button: hovered or focused it glows; true when clicked, or confirmed while focused.
inline bool Button(const Canvas& c, int index, int& focus, bool confirm, float x0, float y0, float x1, float y1,
            const std::string& label, uint32_t color, bool enabled = true, float textSize = 26.0f) {
    const ImVec2 lo = c.P(x0 - 10, y1), hi = c.P(x1 + 10, y0);
    const bool hovered = ImGui::IsMouseHoveringRect(lo, hi);
    if (hovered && enabled && (ImGui::GetIO().MouseDelta.x != 0 || ImGui::GetIO().MouseDelta.y != 0)) focus = index;
    const bool focused = focus == index;
    const float pulse = focused ? 0.5f + 0.5f * std::sin(c.t * 6.0f) : 0.0f;
    const uint32_t body = enabled ? color : 0x3A3F55FFu;
    if (focused && enabled) {
        const float g = 4.0f + pulse * 3.0f;
        c.list->AddQuadFilled(c.P(x0 + 10 - g, y1 + g), c.P(x1 + 10 + g, y1 + g), c.P(x1 - 10 + g, y0 - g),
                              c.P(x0 - 10 - g, y0 - g), Col(0xFFFFFFFFu, 0.55f + 0.35f * pulse));
    }
    Plate(c, x0, y0, x1, y1, body);
    const bool darkText = (color == kPlayer[2] || color == kGold) && enabled;
    Text(c, *TitleFont(), label, (x0 + x1) * 0.5f, (y0 + y1) * 0.5f, textSize, darkText ? 0x2A1A00FFu : kSnow,
         darkText ? 0x4A3000FFu : (enabled ? 0xFFE7B0FFu : kDim), 1, (x1 - x0) - 24.0f, !darkText);
    return enabled && ((hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) || (focused && confirm));
}

// Left/right selector: "<  value  >".
inline bool Selector(const Canvas& c, int index, int& focus, const Nav& nav, float y, const std::string& label,
              const std::string& value, int& option, int count) {
    const bool focused = focus == index;
    const ImVec2 lo = c.P(-300, y + 22), hi = c.P(300, y - 22);
    if (ImGui::IsMouseHoveringRect(lo, hi) && (ImGui::GetIO().MouseDelta.x != 0 || ImGui::GetIO().MouseDelta.y != 0)) focus = index;
    Plate(c, -300, y - 22, 300, y + 22, focused ? 0x2B3350FFu : 0x1C2133FFu, 8.0f);
    Body(c, label, -280, y, 18, kDim, 0);
    Text(c, *TitleFont(), value, 120, y, 22, kSnow, 0xFFE7B0FFu, 1, 230, true);
    const bool arrows = focused || ImGui::IsMouseHoveringRect(lo, hi);
    if (arrows) {
        Arrow(c, -10, y, 18, 2, kOrange);
        Arrow(c, 250, y, 18, 3, kOrange);
    }
    int step = 0;
    if (focused && nav.left) step = -1;
    if (focused && nav.right) step = 1;
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsMouseHoveringRect(lo, hi)) {
        step = ImGui::GetIO().MousePos.x < c.P(120, 0).x ? -1 : 1;
    }
    if (step != 0) option = (option + step + count) % count;
    return step != 0;
}

// Typed characters this frame (names, codes, addresses).
inline void TypeInto(std::string& text, size_t limit, bool codeOnly) {
    ImGuiIO& io = ImGui::GetIO();
    for (ImWchar ch : io.InputQueueCharacters) {
        if (ch < 32 || ch >= 127 || text.size() >= limit) continue;
        char cc = static_cast<char>(ch);
        if (codeOnly && !std::isalnum(static_cast<unsigned char>(cc)) && cc != '.' && cc != ':' && cc != '-') continue;
        text += codeOnly && text.find('.') == std::string::npos ? static_cast<char>(std::toupper(cc)) : cc;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Backspace) && !text.empty()) text.pop_back();
    if (ImGui::IsKeyDown(ImGuiKey_ModCtrl) && ImGui::IsKeyPressed(ImGuiKey_V)) {
        if (const char* clip = ImGui::GetClipboardText()) {
            for (const char* p = clip; *p && text.size() < limit; ++p) {
                if (*p > 32 && *p < 127) text += codeOnly && std::isalpha(static_cast<unsigned char>(*p)) ? static_cast<char>(std::toupper(*p)) : *p;
            }
        }
    }
}


inline void Backdrop(const Canvas& c) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 lo = vp->Pos, hi(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y);
    c.list->AddRectFilledMultiColor(lo, hi, IM_COL32(16, 18, 34, 255), IM_COL32(16, 18, 34, 255), IM_COL32(40, 14, 10, 255),
                                    IM_COL32(40, 14, 10, 255));
    // Slow light rays from behind the banner, as on the title card.
    const ImVec2 origin = c.P(0, 330);
    const float reach = std::max(vp->Size.x, vp->Size.y) * 1.4f;
    for (int i = 0; i < 14; ++i) {
        const float a = c.t * 0.05f + i * 0.4488f;
        const ImVec2 p1(origin.x + std::cos(a) * reach, origin.y + std::sin(a) * reach),
            p2(origin.x + std::cos(a + 0.16f) * reach, origin.y + std::sin(a + 0.16f) * reach);
        c.list->AddTriangleFilled(origin, p1, p2, IM_COL32(255, 160, 70, 14));
    }
}

inline void SmashBall(const Canvas& c, float x, float y, float size, float angle) {
    static ImTextureID texture{};
    static float aspect = 1.0f;
    static bool tried = false;
    if (!tried) {
        tried = true;
        uint32_t w = 0, h = 0;
        if (uint8_t* rgba = aurora_load_png_rgba("startup/smash_ball.png", &w, &h)) {
            texture = aurora_imgui_add_texture(w, h, rgba);
            aspect = h ? float(w) / float(h) : 1.0f;
            std::free(rgba);
        }
    }
    if (!texture) return;
    const float hw = size * aspect * 0.5f, hh = size * 0.5f, co = std::cos(angle), si = std::sin(angle);
    auto Q = [&](float dx, float dy) { return c.P(x + dx * co - dy * si, y + dx * si + dy * co); };
    c.list->AddImageQuad(texture, Q(-hw, hh), Q(hw, hh), Q(hw, -hh), Q(-hw, -hh));
}


} // namespace MenuKit
