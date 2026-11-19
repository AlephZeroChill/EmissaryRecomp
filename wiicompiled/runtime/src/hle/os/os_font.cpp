// NSMBW: system (IPL ROM) font support.
//
// NSMBW's dRomFontMgr_c loads the console's ROM font through nw4r::ut::RomFont, which asks the SDK
// for the font encoding first. OSGetFontEncode reads the VI DTV status register (0xCC00206E) on
// hardware: bit 1 set means a Japanese console (Shift-JIS font), clear means ANSI. Mario Kart Wii
// never loads the ROM font, so the runtime had no answer for that register read.
#include "hle_stubs.h"
#include "memory.h"
#include "runtime_log.h"

#include <cstdint>

// Every SDK font entry point repeats the encoding check with its own inline register read
// (OSSetFontEncode, OSInitFont, OSLoadFont, OSGetFontTexel/Texture/Width), so each one that the game
// can reach without a loaded font needs a native.
namespace {
constexpr uint32_t kOsFontEncodeAnsi = 0;  // OS_FONT_ENCODE_ANSI: NTSC-U / PAL consoles
constexpr uint32_t kOsFontEncodeSjis = 1;  // OS_FONT_ENCODE_SJIS: Japanese consoles
// Brawl keeps the encoding in guest memory, where its translated font code reads it too: a u16 at r13-0x5A40, 0xFFFF
// until first asked, and beside it the string parser its font routines call (r13-0x3D70): 0x801DC7C0 (SJIS/ANSI), or
// 0x801DC8BC (UTF) once an encoding 3-5 is set (OSGetFontEncode 0x801DC2C4, OSSetFontEncode 0x801DC318).
constexpr uint32_t kFontEncodeAddr = 0x8059E9E0u;  // r13-0x5A40
constexpr uint32_t kFontParserAddr = 0x805A06B0u;  // r13-0x3D70
constexpr uint32_t kFontParserSjisAnsi = 0x801DC7C0u;
constexpr uint32_t kFontParserUtf = 0x801DC8BCu;

// The encoding, set on first use as the SDK does, except that its source - the VI DTV select bit on an NTSC console,
// ANSI otherwise - is answered with ANSI: RSBE01 is a North American game, and that bit marks a Japanese console.
uint32_t CurrentFontEncode()
{
    uint16_t encode = Memory::Read16(kFontEncodeAddr);
    if (encode == 0xFFFFu) {
        encode = kOsFontEncodeAnsi;
        Memory::Write16(kFontEncodeAddr, encode);
        Memory::Write32(kFontParserAddr, kFontParserSjisAnsi);
    }
    return encode;
}

void LogOnce(bool& logged, const char* message)
{
    if (!logged) {
        logged = true;
        RT_LOGF(RT_TAG_OS, "%s\n", message);
    }
}
} // namespace

// OSGetFontEncode (Brawl 0x801DC2C4): the current system-font encoding.
extern "C" uint32_t OSGetFontEncode_HLE()
{
    static bool logged = false;
    LogOnce(logged, "OSGetFontEncode: reporting the configured encoding (no VI DTV register read)");
    return CurrentFontEncode();
}
PPC_NATIVE_OVERRIDE(801DC2C4, OSGetFontEncode_HLE, uint32_t, (), ());

// OSSetFontEncode (Brawl 0x801DC318): store an encoding 0-5 (3-5 also select the UTF parser) and return the previous one.
extern "C" uint32_t OSSetFontEncode_HLE(uint32_t encode)
{
    const uint32_t previous = CurrentFontEncode();
    if (encode <= 5) {
        Memory::Write16(kFontEncodeAddr, static_cast<uint16_t>(encode));
        if (encode >= 3) Memory::Write32(kFontParserAddr, kFontParserUtf);
    }
    return previous;
}
PPC_NATIVE_OVERRIDE(801DC318, OSSetFontEncode_HLE, uint32_t, (uint32_t encode), (encode));

// OSInitFont (NSMBW 0x801B0D60) and OSLoadFont (0x801B0480) decode the console's system font, which
// __OSReadROM fetches from the IPL ROM over EXI. That data belongs to the console, not the disc, so
// report "no font" - the SDK's own failure result. nw4r::ut::RomFont::Load then leaves its font
// unloaded, and dRomFontMgr_c continues (it does not check the result). Nintendo games of this era
// draw with the ROM font only on disc-error screens, which the HLE DVD never raises.
extern "C" uint32_t OSInitFont_HLE_801b0d60(uint32_t fontData)
{
    static bool logged = false;
    LogOnce(logged, "OSInitFont: no console ROM font available; reporting failure (RomFont stays unloaded)");
    (void)fontData;
    return 0;
}

extern "C" uint32_t OSLoadFont_HLE(uint32_t fontData, uint32_t temp)
{
    static bool logged = false;
    LogOnce(logged, "OSLoadFont: no console ROM font available; reporting failure");
    (void)fontData;
    (void)temp;
    return 0;
}
PPC_NATIVE_OVERRIDE(801DC698, OSLoadFont_HLE, uint32_t, (uint32_t fontData, uint32_t temp), (fontData, temp));

// OSGetFontTexel (SPM 0x80271428): draws one character of the ROM font into a texture and returns the
// next character. SPM's older SDK inlines the encoding check here as a VI DTV register read. OSLoadFont
// reports no font, so draw nothing: skip one character (two for an SJIS lead byte) and report zero width.
extern "C" uint32_t OSGetFontTexel_HLE(uint32_t string, uint32_t image, int32_t pos, int32_t stride, uint32_t widthPtr)
{
    static bool logged = false;
    LogOnce(logged, "OSGetFontTexel: no console ROM font; characters are skipped");
    (void)image; (void)pos; (void)stride;
    try {
        if (widthPtr != 0) Memory::Write32(widthPtr, 0);
        const uint8_t c = Memory::Read8(string);
        if (c == 0) return string;
        const bool lead = CurrentFontEncode() == kOsFontEncodeSjis && ((c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC));
        return string + (lead ? 2u : 1u);
    } catch (const Memory::AccessViolation&) {
        return string;
    }
}
PPC_NATIVE_OVERRIDE(801DCA78, OSGetFontTexel_HLE, uint32_t,
                    (uint32_t string, uint32_t image, int32_t pos, int32_t stride, uint32_t widthPtr),
                    (string, image, pos, stride, widthPtr));
