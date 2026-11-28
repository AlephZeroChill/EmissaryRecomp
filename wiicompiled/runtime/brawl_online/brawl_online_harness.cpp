// Brawl Online's test harness in EmissaryRecomp (core side, brawl_online_bridge.h): the part of
// Dolphin's harness protocol (brawlonline.net harness/ppharness, Core/Harness/HarnessServer.cpp)
// its online tests drive a game with: memory, pads by the game's pad polls, waits, and the core's
// own status. On when BRAWL_ONLINE_HARNESS_PORT is set; one client at a time, loopback only.

#include "brawl_online_bridge.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <map>
#include <tuple>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <dbghelp.h>

#include "hang_dump_helper.h"
using Socket = SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
using Socket = int;
#define INVALID_SOCKET (-1)
#define closesocket close
#endif

#include <aurora/aurora.h>
#include <picojson.h>
#include <zlib.h>

#include "BrawlOnline/Online/Chat.h"
#include "BrawlOnline/Online/GameBridge.h"
#include "BrawlOnline/Online/GameSetup.h"
#include "BrawlOnline/Online/OnlineClient.h"
#include "BrawlOnline/Online/OnlineSession.h"
#include "BrawlOnline/Online/Rooms.h"
#include "BrawlOnline/Online/Spectate.h"
#include "BrawlOnline/Config/OnlineSettings.h"
#include "BrawlOnline/Rollback/ReplayRecorder.h"
#include "Common/Config/Config.h"
#include "Core/Rollback/ReplayPlayback.h"
#include "BrawlOnline/Rollback/GameplayRollback.h"
#include "BrawlOnline/Rollback/GameplaySession.h"
#include "Common/Logging/Log.h"
#include "BrawlOnline/UI/InputDisplayUI.h"
#include "BrawlOnline/UI/OnScreenDisplay.h"
#include "BrawlOnline/UI/OnlineChatUI.h"

namespace B = BrawlOnlineBridge;
using Object = picojson::object;

namespace {

struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

std::mutex s_mutex;
std::condition_variable s_cv;
std::atomic<uint64_t> s_polls{0};
struct Port {
    std::optional<B::Pad> set;       // pad_set, until pad_clear
    std::vector<B::Pad> script;      // pad_script, one entry per poll
    uint64_t script_start = 0;
    // A script that steps with the host clock (pad_script_timing "time"): entry i from script_t0 + i fields, and
    // before script_t0 what the port presented when it was submitted.
    bool script_timed = false;
    std::chrono::steady_clock::time_point script_t0{};
    std::optional<B::Pad> gap;
};
std::array<Port, 4> s_ports;

// pad_script_timing (Dolphin's Harness::SetScriptTiming): scripts submitted from then on step with the host clock,
// one entry a field (59.94 Hz) from a random fraction of a field after they are submitted, instead of with the
// game's reads, as a player's presses fall anywhere between two reads. Under s_mutex.
bool s_timed_scripts = false;
uint64_t s_timed_rng = 0x9E3779B97F4A7C15ull;
constexpr std::chrono::duration<double, std::micro> kField(1e6 * 1001.0 / 60000.0);

uint64_t NextTimedRandom() {
    // splitmix64, as Dolphin's
    uint64_t z = (s_timed_rng += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

// What port `port` presents at poll `poll` (the poll's own index) and host time `now`: a controller on port 1 only,
// as Dolphin's harness instances have (its other ports read "no controller", error 0xFF), so the game's pad memory
// is the same on both; pad_set over it; a script over that. Under s_mutex.
B::Pad PortPad(int port, uint64_t poll, std::chrono::steady_clock::time_point now) {
    const Port& p = s_ports[size_t(port)];
    B::Pad pad{};
    pad.connected = port == 0;
    if (p.set) pad = *p.set;
    if (p.script.empty()) return pad;
    if (p.script_timed) {
        const double pos = std::chrono::duration<double, std::micro>(now - p.script_t0) / kField;
        if (pos < 0) return p.gap ? *p.gap : pad;
        if (pos < double(p.script.size())) return p.script[size_t(pos)];
        return pad;
    }
    if (poll >= p.script_start && poll < p.script_start + p.script.size()) return p.script[size_t(poll - p.script_start)];
    return pad;
}

picojson::value Num(double v) { return picojson::value(v); }

const picojson::value* Find(const Object& o, const char* key) {
    const auto it = o.find(key);
    return it == o.end() ? nullptr : &it->second;
}
uint64_t U64(const Object& o, const char* key, std::optional<uint64_t> def = std::nullopt) {
    const picojson::value* v = Find(o, key);
    if (!v) {
        if (def) return *def;
        throw Error(std::string("missing '") + key + "'");
    }
    if (v->is<double>()) return uint64_t(v->get<double>());
    if (v->is<std::string>()) return std::stoull(v->get<std::string>(), nullptr, 0);
    throw Error(std::string("'") + key + "' must be a number");
}
std::string Str(const Object& o, const char* key) {
    const picojson::value* v = Find(o, key);
    if (!v || !v->is<std::string>()) throw Error(std::string("'") + key + "' must be a string");
    return v->get<std::string>();
}
int PortOf(const Object& o) {
    const uint64_t p = U64(o, "port");
    if (p > 3) throw Error("port must be 0-3");
    return int(p);
}

uint16_t ButtonMask(const std::string& name) {
    static const std::array<std::pair<const char*, uint16_t>, 12> table{{
        {"A", 0x0100}, {"B", 0x0200}, {"X", 0x0400}, {"Y", 0x0800}, {"Z", 0x0010}, {"L", 0x0040},
        {"R", 0x0020}, {"START", 0x1000}, {"DUP", 0x0008}, {"DDOWN", 0x0004}, {"DLEFT", 0x0001},
        {"DRIGHT", 0x0002},
    }};
    std::string n = name;
    for (auto& ch : n) ch = char(std::toupper(static_cast<unsigned char>(ch)));
    for (const auto& [k, m] : table)
        if (n == k) return m;
    throw Error("unknown button " + name);
}

B::Pad ParsePad(const Object& o) {
    B::Pad pad{};
    pad.connected = true;
    if (const picojson::value* b = Find(o, "buttons")) {
        if (!b->is<picojson::array>()) throw Error("'buttons' must be a list");
        for (const auto& x : b->get<picojson::array>()) pad.button |= ButtonMask(x.get<std::string>());
    }
    auto xy = [&](const char* key, int8_t* x, int8_t* y) {
        const picojson::value* v = Find(o, key);
        if (!v) return;
        const auto& a = v->get<picojson::array>();
        if (a.size() != 2) throw Error(std::string("'") + key + "' must be [x, y]");
        *x = int8_t(int(a[0].get<double>()) - 128);
        *y = int8_t(int(a[1].get<double>()) - 128);
    };
    xy("main", &pad.stickX, &pad.stickY);
    xy("c", &pad.substickX, &pad.substickY);
    pad.triggerL = uint8_t(U64(o, "l", (pad.button & 0x40) ? 255 : 0));
    pad.triggerR = uint8_t(U64(o, "r", (pad.button & 0x20) ? 255 : 0));
    return pad;
}

// screenshot {path}: the next frame the game presents (aurora reads it back from the GPU: the game's
// picture, without the UI over it), as a PNG at `path`. Presents come from the game's thread: in a
// match, at every frame; standing still (a replay paused, a live game buffering), from the core's
// Engine::WhileStandingStill. `captured` false when none came in 5 s.
uint32_t Le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
void PutBe32(std::string& out, uint32_t v) {
    for (int i = 3; i >= 0; --i) out.push_back(char(v >> (8 * i)));
}
void PngChunk(std::string& out, const char* type, const std::string& data) {
    PutBe32(out, uint32_t(data.size()));
    const std::string body = std::string(type, 4) + data;
    out += body;
    PutBe32(out, uint32_t(crc32(0, reinterpret_cast<const Bytef*>(body.data()), uInt(body.size()))));
}
// aurora's capture (write_bmp: 32-bit BGRA rows, bottom-up, 54-byte header) to an RGB PNG.
bool BmpToPng(const std::string& bmp_path, const std::string& png_path, uint32_t* w, uint32_t* h) {
    std::FILE* f = std::fopen(bmp_path.c_str(), "rb");
    if (!f) return false;
    std::vector<uint8_t> bmp;
    uint8_t buf[65536];
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) bmp.insert(bmp.end(), buf, buf + n);
    std::fclose(f);
    if (bmp.size() < 54 || bmp[0] != 'B' || bmp[1] != 'M') return false;
    const uint32_t off = Le32(&bmp[10]), width = Le32(&bmp[18]), height = Le32(&bmp[22]);
    if (width == 0 || height == 0 || width > 16384 || height > 16384 || bmp.size() < off + size_t(width) * height * 4)
        return false;
    std::vector<uint8_t> raw((size_t(width) * 3 + 1) * height);
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* src = &bmp[off + size_t(height - 1 - y) * width * 4];
        uint8_t* dst = &raw[size_t(y) * (size_t(width) * 3 + 1)];
        *dst++ = 0;  // no filter
        for (uint32_t x = 0; x < width; ++x, src += 4) {
            *dst++ = src[2];
            *dst++ = src[1];
            *dst++ = src[0];
        }
    }
    uLongf packed_size = compressBound(uLong(raw.size()));
    std::string packed(packed_size, '\0');
    if (compress2(reinterpret_cast<Bytef*>(packed.data()), &packed_size, raw.data(), uLong(raw.size()), 6) != Z_OK)
        return false;
    packed.resize(packed_size);
    std::string png("\x89PNG\r\n\x1a\n", 8), ihdr;
    PutBe32(ihdr, width);
    PutBe32(ihdr, height);
    ihdr += std::string("\x08\x02\x00\x00\x00", 5);  // 8 bits, RGB
    PngChunk(png, "IHDR", ihdr);
    PngChunk(png, "IDAT", packed);
    PngChunk(png, "IEND", {});
    std::FILE* o = std::fopen(png_path.c_str(), "wb");
    if (!o) return false;
    const bool ok = std::fwrite(png.data(), 1, png.size(), o) == png.size();
    std::fclose(o);
    *w = width;
    *h = height;
    return ok;
}
Object Screenshot(const std::string& path) {
    const std::string bmp = path + ".capture.bmp";
    std::remove(bmp.c_str());
    aurora_request_frame_capture(0, bmp.c_str());
    Object r;
    r["path"] = picojson::value(path);
    uint32_t w = 0, h = 0;
    bool captured = false;
    // The file is written once the GPU is done with the frame; read it once its size holds.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    long last = -1;
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::FILE* f = std::fopen(bmp.c_str(), "rb");
        if (!f) continue;
        std::fseek(f, 0, SEEK_END);
        const long size = std::ftell(f);
        std::fclose(f);
        if (size > 54 && size == last) {
            captured = BmpToPng(bmp, path, &w, &h);
            break;
        }
        last = size;
    }
    std::remove(bmp.c_str());
    r["captured"] = picojson::value(captured);
    r["width"] = Num(double(w));
    r["height"] = Num(double(h));
    return r;
}

std::string Hex(const uint8_t* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s(n * 2, '0');
    for (size_t i = 0; i < n; ++i) { s[2 * i] = d[p[i] >> 4]; s[2 * i + 1] = d[p[i] & 15]; }
    return s;
}

std::vector<uint8_t> FromHex(const std::string& h) {
    if (h.size() % 2) throw Error("odd hex length");
    std::vector<uint8_t> out(h.size() / 2);
    for (size_t i = 0; i < out.size(); ++i) out[i] = uint8_t(std::stoul(h.substr(2 * i, 2), nullptr, 16));
    return out;
}

bool Bool(const Object& o, const char* key, bool def) {
    const picojson::value* v = Find(o, key);
    if (!v) return def;
    if (v->is<bool>()) return v->get<bool>();
    if (v->is<double>()) return v->get<double>() != 0;
    throw Error(std::string("'") + key + "' must be a boolean");
}

// game_pads (HarnessServer.cpp CmdGamePads): the match's pads, recorded and injected per frame.
Object GamePads(const Object& a) {
    if (Find(a, "record")) Gprb::PadsSetRecording(Bool(a, "record", false));
    if (Bool(a, "clear", false)) Gprb::PadsInjectClear();
    if (const picojson::value* rows = Find(a, "inject")) {
        if (!rows->is<picojson::array>()) throw Error("'inject' must be a list of [frame, hex]");
        for (const auto& rv : rows->get<picojson::array>()) {
            if (!rv.is<picojson::array>() || rv.get<picojson::array>().size() != 2 ||
                !rv.get<picojson::array>()[1].is<std::string>())
                throw Error("each inject row must be [frame, hex]");
            const auto& row = rv.get<picojson::array>();
            const std::vector<uint8_t> bytes = FromHex(row[1].get<std::string>());
            if (bytes.size() != Gprb::PAD_SLOTS_SIZE) throw Error("bad inject row");
            Gprb::PadSlots slots;
            std::copy(bytes.begin(), bytes.end(), slots.begin());
            Gprb::PadsInjectAdd(static_cast<u32>(row[0].get<double>()), slots);
        }
    }
    if (Find(a, "ports")) Gprb::PadsInjectSetPorts(static_cast<u32>(U64(a, "ports")));
    if (Find(a, "anchor")) Gprb::PadsSetAnchor(static_cast<u32>(U64(a, "anchor")));
    Object r;
    if (Find(a, "since")) {
        picojson::array out;
        for (const auto& [frame, slots] : Gprb::PadsRecorded(static_cast<u32>(U64(a, "since")))) {
            picojson::array row;
            row.emplace_back(Num(frame));
            row.emplace_back(picojson::value(Hex(slots.data(), slots.size())));
            out.emplace_back(std::move(row));
        }
        r["rows"] = picojson::value(std::move(out));
    }
    r["injected"] = Num(Gprb::PadsInjectedCount());
    return r;
}

// frame_trace (HarnessServer.cpp CmdFrameTrace): [game_frame, persistent, logic_steps, ticks,
// rng x3, fighters_crc, range_hash, game_set, resim, fighters].
Object FrameTrace(const Object& a) {
    std::optional<u32> epoch;
    if (Find(a, "epoch")) epoch = static_cast<u32>(U64(a, "epoch"));
    const auto rows = Gprb::FrameTraceRows(epoch, static_cast<u32>(U64(a, "since", 0)),
                                           static_cast<u32>(U64(a, "max", 0)));
    picojson::array out;
    out.reserve(rows.size());
    char hash[24];
    for (const auto& t : rows) {
        picojson::array row;
        row.emplace_back(Num(t.game_frame));
        row.emplace_back(Num(t.persistent));
        row.emplace_back(Num(t.logic_steps));
        row.emplace_back(Num(double(t.ticks)));
        for (u32 v : t.rng) row.emplace_back(Num(v));
        row.emplace_back(Num(t.fighters_crc));
        std::snprintf(hash, sizeof(hash), "%016llx", (unsigned long long)t.range_hash);
        row.emplace_back(picojson::value(std::string(hash)));
        row.emplace_back(Num(t.game_set));
        row.emplace_back(picojson::value(t.resim));
        picojson::array f;
        for (const auto& port : t.fighters)
            for (u32 v : port) f.emplace_back(Num(v));
        row.emplace_back(picojson::value(std::move(f)));
        out.emplace_back(std::move(row));
    }
    Object r;
    r["epoch"] = Num(Gprb::FrameTraceEpoch());
    r["rows"] = picojson::value(std::move(out));
    return r;
}

Object Status() {
    Object r;
    const uint64_t polls = s_polls.load();
    r["state"] = picojson::value(polls > 0 ? "running" : "starting");
    r["frame"] = Num(double(polls));
    r["input_polls"] = Num(double(polls));
    r["poll_source"] = picojson::value("recomp");
    r["si_polls"] = Num(double(polls));
    r["game_id"] = picojson::value("RSBE01");
    r["cpu_thread"] = picojson::value(false);
    r["video_backend"] = picojson::value("EmissaryRecomp");
    r["audio_muted"] = picojson::value(true);
    r["engine"] = picojson::value("emissary");
    r["netplay"] = picojson::value();
    return r;
}

Object Dispatch(const std::string& cmd, const Object& a) {
    if (cmd == "ping" || cmd == "hello") {
        Object r;
        r["pong"] = picojson::value(true);
        r["version"] = Num(1);  // Dolphin's harness protocol, v1
        return r;
    }
    if (cmd == "status") return Status();
    if (cmd == "read_mem" || cmd == "read_u32") {
        const uint32_t addr = uint32_t(U64(a, "addr"));
        const uint32_t len = cmd == "read_u32" ? 4 : uint32_t(U64(a, "len"));
        const uint8_t* p = B::GuestPointer(addr, len);
        if (!p) throw Error("memory not accessible");
        Object r;
        if (cmd == "read_u32") r["value"] = Num(double(uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]));
        else r["hex"] = picojson::value(Hex(p, len));
        return r;
    }
    if (cmd == "write_mem") {
        const uint32_t addr = uint32_t(U64(a, "addr"));
        const std::string h = Str(a, "hex");
        if (h.size() % 2) throw Error("odd hex length");
        uint8_t* p = B::GuestPointer(addr, uint32_t(h.size() / 2));
        if (!p) throw Error("memory not accessible");
        for (size_t i = 0; i < h.size() / 2; ++i) p[i] = uint8_t(std::stoul(h.substr(2 * i, 2), nullptr, 16));
        B::MarkDirty(addr, uint32_t(h.size() / 2));
        return {};
    }
    if (cmd == "pad_set") {
        std::lock_guard lock(s_mutex);
        s_ports[PortOf(a)].set = ParsePad(a);
        return {};
    }
    if (cmd == "pad_clear") {
        std::lock_guard lock(s_mutex);
        auto& p = s_ports[PortOf(a)];
        p.set.reset();
        p.script.clear();
        return {};
    }
    if (cmd == "pad_script") {
        const int port = PortOf(a);
        std::vector<B::Pad> timeline;
        for (const auto& f : a.at("frames").get<picojson::array>()) {
            const Object& fo = f.get<Object>();
            const B::Pad pad = ParsePad(fo);
            const uint64_t hold = U64(fo, "hold", 1);
            timeline.insert(timeline.end(), hold, pad);
        }
        std::lock_guard lock(s_mutex);
        auto& p = s_ports[port];
        uint64_t start = s_polls.load() + 1;
        const picojson::value* sv = Find(a, "start");
        const bool given = sv && sv->is<double>();
        if (given) start = uint64_t(sv->get<double>());
        // A timed script bridges the time until it starts with what the port presents now.
        const auto now = std::chrono::steady_clock::now();
        const B::Pad before = PortPad(port, s_polls.load(), now);
        p.script = std::move(timeline);
        p.script_start = start;
        p.script_timed = s_timed_scripts && !given;
        if (p.script_timed) {
            p.gap = before;
            const double frac = double(NextTimedRandom() >> 11) * 0x1p-53;  // [0, 1)
            p.script_t0 = now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(kField * frac);
        }
        Object r;
        r["starts_at"] = Num(double(start));
        r["ends_at"] = Num(double(start + p.script.size()));
        return r;
    }
    if (cmd == "pad_script_status") {
        std::lock_guard lock(s_mutex);
        const auto& p = s_ports[PortOf(a)];
        const uint64_t polls = s_polls.load();
        const uint64_t end = p.script_start + p.script.size();
        Object r;
        if (p.script_timed && !p.script.empty()) {
            const double pos =
                std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - p.script_t0) / kField;
            const double left = double(p.script.size()) - std::max(pos, 0.0);
            r["active"] = picojson::value(left > 0);
            r["remaining"] = Num(left > 0 ? std::ceil(left) : 0.0);
            return r;
        }
        r["active"] = picojson::value(!p.script.empty() && polls < end);
        r["remaining"] = Num(double(!p.script.empty() && polls < end ? end - polls : 0));
        return r;
    }
    if (cmd == "pad_script_timing") {
        // Dolphin's (docs/harness-protocol.md): "time" (scripts step with the host clock) or "poll" (with the game's reads).
        const std::string mode = Find(a, "mode") ? Str(a, "mode") : "poll";
        if (mode != "time" && mode != "poll") throw Error("mode must be \"time\" or \"poll\"");
        std::lock_guard lock(s_mutex);
        s_timed_scripts = mode == "time";
        s_timed_rng = U64(a, "seed", 0) ^ 0x9E3779B97F4A7C15ull;
        return {};
    }
    if (cmd == "wait_frame") {
        const bool polls = Find(a, "input_polls") != nullptr;
        const uint64_t target = U64(a, polls ? "input_polls" : "frame");
        const auto timeout = std::chrono::milliseconds(U64(a, "timeout_ms", 10000));
        std::unique_lock lock(s_mutex);
        if (!s_cv.wait_for(lock, timeout, [&] { return s_polls.load() >= target; })) throw Error("timeout");
        Object r;
        r["frame"] = Num(double(s_polls.load()));
        r["input_polls"] = Num(double(s_polls.load()));
        return r;
    }
    if (cmd == "gprb_status") return Gprb::Session::Status().get<Object>();
    // Dolphin's HarnessServer.cpp CmdGprbSyncTest / CmdGprbStop, the same core calls: a GekkoNet stress session (or a
    // pass log's replay) on this game, so the sync tests and rollback benchmarks run on both engines.
    if (cmd == "gprb_synctest") {
        Gprb::Session::SyncTestOptions o;
        o.distance = int(U64(a, "distance", 2));
        if (Find(a, "region_set")) o.region_set = Str(a, "region_set");
        o.hash_regions = Bool(a, "hash_regions", true);
        o.start_frame = static_cast<u32>(U64(a, "start_frame", o.start_frame));
        o.ports = static_cast<u32>(U64(a, "ports", 3));
        o.suppress_resim_sounds = Bool(a, "suppress_resim_sounds", false);
        o.dedupe_resim_sounds = Bool(a, "dedupe_resim_sounds", false);
        o.inject_input = Bool(a, "inject_input", false);
        o.mispredict_ports = static_cast<u32>(U64(a, "mispredict_ports", 0));
        if (const picojson::value* v = Find(a, "mispredict_offset"); v && v->is<double>())
            o.mispredict_offset = int(v->get<double>());  // signed (U64 takes no negative numbers)
        o.mispredict_every = int(U64(a, "mispredict_every", 1));
        if (Find(a, "replay_path")) o.replay_path = Str(a, "replay_path");
        o.no_rollback = Bool(a, "no_rollback", false);
        if (const auto err = Gprb::Session::ArmSyncTest(o)) throw Error(*err);
        return Gprb::Session::Status().get<Object>();
    }
    if (cmd == "gprb_stop") {
        Gprb::Session::Stop();
        return Gprb::Session::Status().get<Object>();
    }
    // The online tests' comparisons (Dolphin's HarnessServer.cpp, the same core calls).
    if (cmd == "gprb_checksums")
        return Gprb::Session::Checksums(static_cast<s64>(U64(a, "since", 0))).get<Object>();
    if (cmd == "game_pads") return GamePads(a);
    if (cmd == "frame_trace") return FrameTrace(a);
    if (cmd == "frame_trace_config") {
        std::vector<std::pair<u32, u32>> ranges;
        if (const picojson::value* r = Find(a, "ranges"); r && r->is<picojson::array>()) {
            for (const auto& e : r->get<picojson::array>()) {
                const auto& v = e.get<picojson::array>();
                if (v.size() != 2) throw Error("ranges entries must be [addr, size]");
                ranges.emplace_back(static_cast<u32>(v[0].get<double>()), static_cast<u32>(v[1].get<double>()));
            }
        }
        Gprb::FrameTraceConfigure(Bool(a, "enabled", true), std::move(ranges));
        return Object{};
    }
    if (cmd == "game_bridge_status") return Online::GameBridge::Status();
    if (cmd == "rooms_status") return Online::Rooms::Status();
    // The room CSS's ops as a test sends them (Dolphin's CmdRoomsRequest; the launcher's requests too),
    // and Rooms' watch requests (watch_leave, watch_hold, ...: Rooms::HarnessRequest).
    if (cmd == "rooms_request") {
        if (const auto error = Online::Rooms::HarnessRequest(a)) throw Error(*error);
        return Online::Rooms::Status();
    }
    // A stage step acted on as the stage select would (Dolphin's CmdGameSetupAct).
    if (cmd == "game_setup_act") {
        const uint64_t kind = U64(a, "kind");
        if (kind < 1 || kind > 0xFF) throw Error("kind must be a stage kind (1-255)");
        const uint64_t asl = U64(a, "asl", 0);
        if (asl > 0xFF) throw Error("asl must be 0-255");
        Object r;
        r["ok"] = picojson::value(Online::GameSetup::Act(uint8_t(kind), uint8_t(asl)));
        r["setup"] = picojson::value(Online::GameSetup::Status());
        return r;
    }
    // Replays and spectating (Dolphin's HarnessServer.cpp: replay_play, replay_comm, replay_status,
    // replay_control, replay_recording, spectate_status, spectate_config; the same core calls).
    if (cmd == "replay_play") {
        if (const auto error = Gprb::Replay::Playback::PlayFile(Str(a, "path"), Bool(a, "play_anyway", false),
                                                               Bool(a, "hold_at_end", false)))
            throw Error(*error);
        return Gprb::Replay::Playback::Status();
    }
    if (cmd == "replay_comm") {
        Gprb::Replay::Playback::SetCommFile(Str(a, "path"));
        return Gprb::Replay::Playback::Status();
    }
    if (cmd == "replay_status") {
        Object r = Gprb::Session::PlaybackStatus();
        r["comm"] = picojson::value(Gprb::Replay::Playback::Status());
        return r;
    }
    if (cmd == "replay_control") {
        using Gprb::Session::PlaybackCommand;
        const std::string action = Str(a, "action");
        double arg = 0;
        if (const picojson::value* v = Find(a, "arg")) {
            if (!v->is<double>()) throw Error("'arg' must be a number");
            arg = v->get<double>();
        }
        static const std::pair<const char*, PlaybackCommand> commands[] = {
            {"pause", PlaybackCommand::Pause},        {"play", PlaybackCommand::Play},
            {"toggle", PlaybackCommand::TogglePause}, {"step", PlaybackCommand::StepForward},
            {"back", PlaybackCommand::StepBack},      {"jump", PlaybackCommand::Jump},
            {"seek", PlaybackCommand::Seek},          {"speed", PlaybackCommand::Speed},
            {"faster", PlaybackCommand::Faster},      {"slower", PlaybackCommand::Slower},
            {"live", PlaybackCommand::GoLive},        {"highlight", PlaybackCommand::PlayHighlight},
        };
        if (action == "stop") {
            Gprb::Session::SetPlaybackStop(static_cast<s64>(arg));
            return Gprb::Session::PlaybackStatus();
        }
        for (const auto& [name, command] : commands) {
            if (action == name) {
                Gprb::Session::PlaybackControl(command, arg);
                return Gprb::Session::PlaybackStatus();
            }
        }
        throw Error("unknown replay command " + action);
    }
    // The replay being recorded (Dolphin's CmdReplayRecording).
    if (cmd == "replay_recording") {
        if (Bool(a, "flush", false)) Gprb::Replay::Flush();
        Object r;
        r["path"] = picojson::value(Gprb::Replay::CurrentPath());
        r["recording"] = picojson::value(Gprb::Replay::Active());
        return r;
    }
    if (cmd == "log_mark") {
        NOTICE_LOG_FMT(BRAWLBACK, "[HARNESS] {}", Str(a, "text"));
        return {};
    }
    if (cmd == "spectate_status") return Online::Spectate::Status();
    if (cmd == "spectate_config") {
        if (Find(a, "stream")) Config::SetCurrent(Config::ONLINE_STREAM_GAMES, Bool(a, "stream", true));
        if (Find(a, "forced")) Online::Spectate::SetForced(Bool(a, "forced", false));
        if (const picojson::value* v = Find(a, "start_rate"))
            Online::Spectate::SetStartRate(v->is<picojson::null>() ? std::nullopt
                                                                   : std::optional<double>(double(U64(a, "start_rate"))));
        Object r = Online::Spectate::Status();
        r["stream"] = picojson::value(Config::Get(Config::ONLINE_STREAM_GAMES));
        return r;
    }
    // Chat and the UI (Dolphin's HarnessServer.cpp, the same core and core/ui calls).
    if (cmd == "chat_status") return Online::Chat::Status();
    if (cmd == "chat_send" || cmd == "chat_hide" || cmd == "chat_report" || cmd == "chat_leave") {
        if (const auto error = Online::Chat::HarnessRequest(cmd, a)) throw Error(*error);
        return Online::Chat::Status();
    }
    if (cmd == "osd_messages") {
        picojson::array texts;
        for (std::string& t : OSD::GetMessageTexts()) texts.emplace_back(std::move(t));
        Object r;
        r["messages"] = picojson::value(texts);
        return r;
    }
    if (cmd == "input_display") {
        // The input display as its last frame drew it (core/ui InputDisplayUI, as Dolphin's).
        picojson::value v;
        const std::string err = picojson::parse(v, BrawlOnline::UI::InputDisplay::Status());
        if (!err.empty() || !v.is<Object>()) throw Error("input display status: " + err);
        return v.get<Object>();
    }
    if (cmd == "chat_ui") {
        // The chat window as the UI drew it: each frame recorded since the last call (a subset of
        // Dolphin's fields), then `record` (default true) and `activate` (the chat key).
        picojson::array frames;
        for (const auto& f : VideoCommon::OnlineChatUI::TakeRecordedFrames()) {
            Object o;
            o["frame"] = Num(f.frame);
            o["display"] = picojson::value(picojson::array{Num(f.display_w), Num(f.display_h)});
            o["shown"] = picojson::value(f.shown);
            o["rect"] = picojson::value(picojson::array{Num(f.x), Num(f.y), Num(f.w), Num(f.h)});
            o["collapsed"] = picojson::value(f.collapsed);
            o["focused"] = picojson::value(f.focused);
            o["alpha"] = Num(f.alpha);
            o["typing"] = picojson::value(f.typing);
            o["title"] = picojson::value(f.title);
            o["unread"] = Num(f.unread);
            o["font_px"] = Num(f.font_px);
            frames.emplace_back(std::move(o));
        }
        VideoCommon::OnlineChatUI::SetRecording(Bool(a, "record", true));
        if (Bool(a, "activate", false)) VideoCommon::OnlineChatUI::Activate();
        Object r;
        r["frames"] = picojson::value(frames);
        return r;
    }
    if (cmd == "online_status") return Online::Client::OnlineStatus();
    if (cmd == "mm_status") return Online::Client::MatchmakingStatus();
    if (cmd == "mm_cancel") {
        Online::Client::Cleanup();
        return Online::Client::MatchmakingStatus();
    }
    if (cmd == "online_session_backend") {
        if (const auto error = Online::Session::Select(Str(a, "backend"))) throw Error(*error);
        return Online::Client::MatchmakingStatus();
    }
    if (cmd == "netplay_status") {
        Object r;
        r["role"] = picojson::value();
        r["connected"] = picojson::value(false);
        r["players"] = picojson::value(picojson::array());
        r["game_running"] = picojson::value(false);
        r["rollback"] = picojson::value();
        return r;
    }
    if (cmd == "screenshot") return Screenshot(Str(a, "path"));
    // emu_stop is closing the game's window (Dolphin: MainWindow::ForceStop); the recomp's window
    // closing ends the program after the core's stop, as quit does.
    if (cmd == "quit" || cmd == "stop_game" || cmd == "emu_stop") {
        std::thread([] { std::this_thread::sleep_for(std::chrono::milliseconds(100)); B::StopGame(); }).detach();
        return {};
    }
    throw Error("unknown command " + cmd);
}

// One connection: JSON lines in, JSON lines out (Dolphin's harness framing).
void ServeClient(Socket c) {
    int yes = 1;
    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&yes), sizeof(yes));
    std::fprintf(stderr, "[brawl-online] harness: client connected\n");
    std::string buf;
    char chunk[65536];
    int n = 0;
    for (;;) {
        n = int(recv(c, chunk, sizeof(chunk), 0));
        if (n <= 0) break;
        buf.append(chunk, size_t(n));
        size_t nl;
        while ((nl = buf.find('\n')) != std::string::npos) {
            const std::string line = buf.substr(0, nl);
            buf.erase(0, nl + 1);
            picojson::value req;
            const std::string perr = picojson::parse(req, line);
            Object resp;
            if (!perr.empty() || !req.is<Object>()) {
                resp["ok"] = picojson::value(false);
                resp["error"] = picojson::value("request must be a JSON object");
            } else {
                const Object& o = req.get<Object>();
                if (const auto* id = Find(o, "id")) resp["id"] = *id;
                try {
                    const auto cmd = o.find("cmd");
                    resp["result"] = picojson::value(Dispatch(cmd != o.end() ? cmd->second.to_str() : "", o));
                    resp["ok"] = picojson::value(true);
                } catch (const std::exception& e) {
                    resp["ok"] = picojson::value(false);
                    resp["error"] = picojson::value(std::string(e.what()));
                } catch (...) {
                    resp["ok"] = picojson::value(false);
                    resp["error"] = picojson::value("unknown error");
                }
            }
            const std::string out = picojson::value(resp).serialize() + "\n";
            size_t sent = 0;
            while (sent < out.size()) {
                const int k = int(send(c, out.data() + sent, int(out.size() - sent), 0));
                if (k <= 0) break;
                sent += size_t(k);
            }
        }
    }
#ifdef _WIN32
    const int err = n < 0 ? WSAGetLastError() : 0;
#else
    const int err = n < 0 ? errno : 0;
#endif
    std::fprintf(stderr, "[brawl-online] harness: client gone (recv %d, error %d)\n", n, err);
    closesocket(c);
}

void Serve(uint16_t port) {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    const Socket listener = socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(listener, 8) != 0) {
        std::fprintf(stderr, "[brawl-online] harness: cannot listen on 127.0.0.1:%u\n", port);
        return;
    }
    std::fprintf(stderr, "[brawl-online] harness listening on 127.0.0.1:%u\n", port);
    for (;;) {
        const Socket c = accept(listener, nullptr, nullptr);
        if (c == INVALID_SOCKET) continue;
        std::thread(ServeClient, c).detach();
    }
}

}  // namespace

#ifdef _WIN32
namespace {
// An exception nothing before this handles (the runtime's own handlers run first): what and where,
// and a minidump beside the user folder's logs, for lldb -c.
LONG CALLBACK LastChanceHandler(EXCEPTION_POINTERS* e) {
    const DWORD code = e->ExceptionRecord->ExceptionCode;
    if (code == 0x406D1388 || code == 0x40010006 || code == 0x4001000A || code == DBG_PRINTEXCEPTION_C ||
        code == 0xE06D7363 || code == 0x20474343 || (code & 0xF0000000) == 0x40000000)
        return EXCEPTION_CONTINUE_SEARCH;  // thread names, debug prints, C++ throws (MSVC's, libgcc's "GCC "): not crashes
    static std::atomic<int> once{0};
    if (once.fetch_add(1) > 3) return EXCEPTION_CONTINUE_SEARCH;
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto at = reinterpret_cast<uintptr_t>(e->ExceptionRecord->ExceptionAddress);
    std::fprintf(stderr, "[brawl-online] exception 0x%08lx at 0x%llx (exe+0x%llx) thread %lu", code,
                 (unsigned long long)at, (unsigned long long)(at - base), GetCurrentThreadId());
    if (code == EXCEPTION_ACCESS_VIOLATION && e->ExceptionRecord->NumberParameters >= 2)
        std::fprintf(stderr, " (%s 0x%llx)", e->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
                     (unsigned long long)e->ExceptionRecord->ExceptionInformation[1]);
    void* frames[48];
    const USHORT n = RtlCaptureStackBackTrace(0, 48, frames, nullptr);
    std::fputs(" stack:", stderr);
    for (USHORT i = 0; i < n; ++i)
        std::fprintf(stderr, " 0x%llx", (unsigned long long)(reinterpret_cast<uintptr_t>(frames[i]) - base));
    std::fputs("\n", stderr);
    std::fflush(stderr);
    BrawlOnlineBridge::DumpGuestCpu();
    if (const char* dir = std::getenv("BRAWL_ONLINE_CORE")) {
        char path[1024];
        std::snprintf(path, sizeof(path), "%s\\Logs\\recomp-%lu.dmp", dir, GetCurrentProcessId());
        const HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION mei{GetCurrentThreadId(), e, FALSE};
            MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f,
                              MINIDUMP_TYPE(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory), &mei,
                              nullptr, nullptr);
            CloseHandle(f);
            std::fprintf(stderr, "[brawl-online] minidump %s\n", path);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
}  // namespace
#endif

namespace BrawlOnlineBridge {

void CoreWatchCrashes() {
#ifdef _WIN32
    AddVectoredExceptionHandler(0, LastChanceHandler);
#endif
}

void CoreWatchExit() {
#ifdef _WIN32
    // Who ends the process: exit()'s caller as offsets in the executable (llvm-symbolizer).
    std::atexit([] {
        void* frames[48];
        const USHORT n = RtlCaptureStackBackTrace(0, 48, frames, nullptr);
        const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        std::fprintf(stderr, "[brawl-online] exit() on thread %lu, stack:", GetCurrentThreadId());
        for (USHORT i = 0; i < n; ++i)
            std::fprintf(stderr, " 0x%llx", (unsigned long long)(reinterpret_cast<uintptr_t>(frames[i]) - base));
        std::fputs("\n", stderr);
        std::fflush(stderr);
    });
#endif
}

void CoreHoldWinsock() {
#ifdef _WIN32
    // Winsock is reference counted per process: a WSACleanup more than its WSAStartups (ENet and
    // Dolphin's SocketContext pair them per use) closes every socket the program has, the game's
    // own included. Dolphin's frontend holds references for its whole run; so does this.
    WSADATA wsa;
    for (int i = 0; i < 64; ++i) WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
}

// BRAWL_ONLINE_PROFILE=1: a sampling profile of the game's thread (the one at the loop top) into
// <BRAWL_ONLINE_CORE>/Logs/profile-<pid>.txt every 5 s: per function (start address in its module,
// from the x64 unwind tables), samples in it (self) and with it on the stack (total). Resolve the
// executable's offsets with WiiCompiled.map.
void CoreProfileStart() {
#ifdef _WIN32
    static std::atomic<bool> started{false};
    if (!std::getenv("BRAWL_ONLINE_PROFILE") || started.exchange(true)) return;
    HANDLE target = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &target,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0))
        return;
    const char* dir = std::getenv("BRAWL_ONLINE_CORE");
    char path[1024];
    std::snprintf(path, sizeof(path), "%s\\Logs\\profile-%lu.txt", dir ? dir : ".", GetCurrentProcessId());
    std::thread([target, file = std::string(path)] {
        struct Count { uint64_t self = 0, total = 0; };
        std::unordered_map<uint64_t, Count> counts;
        // Samples whose leaf is outside the executable (ntdll, ucrtbase, libc++): by (leaf function, the first
        // frame back inside the executable), so a wait or a memcpy can be told by who made it.
        // key: leaf, the first frame in a module other than ntdll/KERNELBASE/kernel32 (who asked the OS: SDL3,
        // xinput, hid...), the first frame back in the executable.
        std::map<std::tuple<uint64_t, uint64_t, uint64_t>, uint64_t> outside;
        HMODULE exeModule = GetModuleHandleA(nullptr);
        const DWORD64 osModules[] = {reinterpret_cast<DWORD64>(GetModuleHandleA("ntdll.dll")),
                                     reinterpret_cast<DWORD64>(GetModuleHandleA("KERNELBASE.dll")),
                                     reinterpret_cast<DWORD64>(GetModuleHandleA("KERNEL32.DLL"))};
        uint64_t samples = 0;
        auto written = std::chrono::steady_clock::now();
        std::vector<uint64_t> seen;
        for (;;) {
            Sleep(1);
            if (SuspendThread(target) == DWORD(-1)) return;
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(target, &ctx)) {
                ++samples;
                seen.clear();
                uint64_t leaf = 0, via = 0;
                bool leafOutside = false, attributed = false;
                for (int depth = 0; depth < 64 && ctx.Rip; ++depth) {
                    DWORD64 image = 0;
                    PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &image, nullptr);
                    const uint64_t start = fn ? image + fn->BeginAddress : ctx.Rip;
                    if (depth == 0) {
                        ++counts[start].self;
                        leaf = start;
                        leafOutside = image != reinterpret_cast<DWORD64>(exeModule);
                    } else if (leafOutside && !attributed && image == reinterpret_cast<DWORD64>(exeModule)) {
                        ++outside[{leaf, via ? via : leaf, start}];
                        attributed = true;
                    } else if (leafOutside && !via && image != osModules[0] && image != osModules[1] &&
                               image != osModules[2]) {
                        via = start;
                    }
                    if (std::find(seen.begin(), seen.end(), start) == seen.end()) {
                        seen.push_back(start);
                        ++counts[start].total;
                    }
                    if (!fn) {
                        ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
                        ctx.Rsp += 8;
                        continue;
                    }
                    void* handler_data = nullptr;
                    DWORD64 frame = 0;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, ctx.Rip, fn, &ctx, &handler_data, &frame, nullptr);
                }
            }
            ResumeThread(target);
            const auto now = std::chrono::steady_clock::now();
            if (now - written < std::chrono::seconds(5)) continue;
            written = now;
            std::vector<std::pair<uint64_t, Count>> rows(counts.begin(), counts.end());
            std::sort(rows.begin(), rows.end(), [](const auto& x, const auto& y) { return x.second.total > y.second.total; });
            if (std::FILE* f = std::fopen(file.c_str(), "w")) {
                std::fprintf(f, "samples %llu\n", (unsigned long long)samples);
                for (const auto& [addr, c] : rows) {
                    HMODULE mod = nullptr;
                    char name[MAX_PATH] = "?";
                    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                           reinterpret_cast<LPCSTR>(addr), &mod))
                        GetModuleFileNameA(mod, name, sizeof(name));
                    const char* base = std::strrchr(name, '\\');
                    std::fprintf(f, "%8llu %8llu %s+0x%llx\n", (unsigned long long)c.total, (unsigned long long)c.self,
                                 base ? base + 1 : name, (unsigned long long)(addr - reinterpret_cast<uintptr_t>(mod)));
                }
                std::vector<std::pair<uint64_t, std::tuple<uint64_t, uint64_t, uint64_t>>> pairs;
                for (const auto& [key, n] : outside) pairs.push_back({n, key});
                std::sort(pairs.rbegin(), pairs.rend());
                const auto name = [](uint64_t addr, char* out, size_t size) {
                    HMODULE mod = nullptr;
                    char path[MAX_PATH] = "?";
                    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                           reinterpret_cast<LPCSTR>(addr), &mod))
                        GetModuleFileNameA(mod, path, sizeof(path));
                    const char* base = std::strrchr(path, '\\');
                    std::snprintf(out, size, "%s+0x%llx", base ? base + 1 : path,
                                  (unsigned long long)(addr - reinterpret_cast<uintptr_t>(mod)));
                };
                for (size_t i = 0; i < pairs.size() && i < 200; ++i) {
                    char a[MAX_PATH + 32], v[MAX_PATH + 32], b[MAX_PATH + 32];
                    name(std::get<0>(pairs[i].second), a, sizeof a);
                    name(std::get<1>(pairs[i].second), v, sizeof v);
                    name(std::get<2>(pairs[i].second), b, sizeof b);
                    std::fprintf(f, "C %8llu %s %s %s\n", (unsigned long long)pairs[i].first, a, v, b);
                }
                std::fclose(f);
            }
        }
    }).detach();
#endif
}

// The game's thread (the loop top's), for CoreGameThreadStack from the core's network thread.
#ifdef _WIN32
static std::atomic<HANDLE> s_gameThread{nullptr};
#endif

void CoreNoteGameThread() {
#ifdef _WIN32
    if (s_gameThread.load()) return;
    HANDLE h = nullptr;
    if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h,
                        THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0))
        s_gameThread.store(h);
#endif
}

#ifdef _WIN32
namespace {
// A thread's native stack now, as "<label> <module>+0x<offset> < ..." (the executable's or engine DLL's offsets
// resolve with its map, or llvm-symbolizer on the released engine_usa.dll, which keeps its symbol table). The thread
// is suspended only while its frames are unwound into a fixed array (nothing allocates meanwhile: it may hold the
// heap's lock), then named after it runs again.
void ThreadStack(HANDLE target, const char* label, char* out, size_t size) {
    if (size) out[0] = 0;
    if (!target || size == 0) return;
    std::array<uint64_t, 40> pcs{};
    size_t n = 0;
    if (SuspendThread(target) == DWORD(-1)) return;
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_FULL;
    if (GetThreadContext(target, &ctx)) {
        while (n < pcs.size() && ctx.Rip) {
            pcs[n++] = ctx.Rip;
            DWORD64 image = 0;
            PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &image, nullptr);
            if (!fn) {  // a leaf: its return address is at the stack pointer
                ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
                ctx.Rsp += 8;
                continue;
            }
            void* handler_data = nullptr;
            DWORD64 frame = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, ctx.Rip, fn, &ctx, &handler_data, &frame, nullptr);
        }
    }
    ResumeThread(target);
    size_t used = std::snprintf(out, size, "%s", label);
    for (size_t i = 0; i < n && used + 1 < size; ++i) {
        HMODULE mod = nullptr;
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(pcs[i]), &mod))
            GetModuleFileNameA(mod, name, sizeof(name));
        const char* base = std::strrchr(name, '\\');
        const int w = std::snprintf(out + used, size - used, "%s %s+0x%llx", i ? " <" : "", base ? base + 1 : name,
                                    (unsigned long long)(pcs[i] - reinterpret_cast<uintptr_t>(mod)));
        if (w < 0) break;
        used += size_t(w);
    }
}

// This process's visible top-level windows (the game's; the F10 Controllers window is Dolphin's own process).
std::vector<HWND> OwnWindows() {
    std::vector<HWND> found;
    EnumWindows(
        [](HWND hwnd, LPARAM lp) -> BOOL {
            DWORD owner = 0;
            GetWindowThreadProcessId(hwnd, &owner);
            if (owner == GetCurrentProcessId() && IsWindowVisible(hwnd))
                reinterpret_cast<std::vector<HWND>*>(lp)->push_back(hwnd);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&found));
    return found;
}

// The minidump of this process by a second one (hang_dump_helper.h: this program with --write-hang-dump), into
// <BRAWL_ONLINE_CORE>/Logs/recomp-<pid>.dmp, where the launcher's bug report takes the core's crash dumps from.
// Its exit code (0 written), or -1 when it could not be started or did not finish in a minute.
long WriteHangDump(std::wstring& path) {
    const wchar_t* dir = _wgetenv(L"BRAWL_ONLINE_CORE");
    if (!dir) return -1;
    path = std::wstring(dir) + L"\\Logs\\recomp-" + std::to_wstring(GetCurrentProcessId()) + L".dmp";
    wchar_t exe[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return -1;
    std::wstring command = L"\"" + std::wstring(exe) + L"\" " + HangDumpHelper::kSwitchW + L" " +
                           std::to_wstring(GetCurrentProcessId()) + L" \"" + path + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return -1;
    long code = -1;
    if (WaitForSingleObject(pi.hProcess, 60000) == WAIT_OBJECT_0) {
        DWORD exit = 0;
        if (GetExitCodeProcess(pi.hProcess, &exit)) code = static_cast<long>(exit);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code;
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
}  // namespace
#endif

// The game's thread's native stack now: for the core's loop watch, when the game's thread has not run a session
// frame for a second.
void CoreGameThreadStack(char* out, size_t size) {
    if (size) out[0] = 0;
#ifdef _WIN32
    ThreadStack(s_gameThread.load(), "game thread stack", out, size);
#endif
}

// The window watchdog. Windows calls a window "Not responding" once its thread has handled no message for 5 s, and a
// player who then closes it ends the game with 0xCFFFFFFF, leaving nothing behind: no crash, no log line, the core's
// loop watch only runs in a match (Brawl Online bug report #382: frozen on the character select after a game, while
// chatting). The recomp handles its window's messages on the game's thread, so a window hung for HANG_SECONDS is the
// game's thread stuck: that thread's native stack goes to the log (console.log and the core's dolphin.log), and once a
// run a minidump of the process (every thread's stack) goes where the bug report takes the core's crash dumps from. A
// window that responds again is logged too. Checked once a second, from its own thread.
void CoreWatchHangs() {
#ifdef _WIN32
    static std::atomic<bool> started{false};
    if (started.exchange(true) || std::getenv("BRAWL_NO_HANG_WATCH")) return;
    std::thread([] {
        constexpr int HANG_SECONDS = 8;  // as Dolphin's HangWatchdog: Windows says hung after 5 s, then 3 more
        using Clock = std::chrono::steady_clock;
        std::optional<Clock::time_point> hung_since;  // when IsHungAppWindow first said so (5 s into the hang)
        bool reported = false, dumped = false;
        for (;;) {
            Sleep(1000);
            HWND hung = nullptr;
            for (HWND hwnd : OwnWindows()) {
                if (IsHungAppWindow(hwnd)) {
                    hung = hwnd;
                    break;
                }
            }
            const auto now = Clock::now();
            if (!hung) {
                if (reported) {
                    const double s = std::chrono::duration<double>(now - *hung_since).count() + 5.0;
                    std::fprintf(stderr, "[brawl-online] hang watch: the window responds again, after about %.0f s\n", s);
                    std::fflush(stderr);
                    WARN_LOG_FMT(BRAWLBACK, "hang watch: the window responds again, after about {:.0f} s", s);
                }
                hung_since.reset();
                reported = false;
                continue;
            }
            if (!hung_since) hung_since = now;
            if (reported || now - *hung_since < std::chrono::seconds(HANG_SECONDS - 5)) continue;
            reported = true;
            const DWORD tid = GetWindowThreadProcessId(hung, nullptr);
            char stack[2048];
            const HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                                             FALSE, tid);
            ThreadStack(thread, "window thread stack", stack, sizeof(stack));
            if (thread) CloseHandle(thread);
            const bool game = s_gameThread.load() && GetThreadId(s_gameThread.load()) == tid;
            std::fprintf(stderr, "[brawl-online] hang watch: the window has not responded for %d s (thread %lu%s); %s\n",
                         HANG_SECONDS, tid, game ? ", the game's" : "", stack);
            std::fflush(stderr);
            WARN_LOG_FMT(BRAWLBACK, "hang watch: the window has not responded for {} s (thread {}{}); {}", HANG_SECONDS,
                         tid, game ? ", the game's" : "", stack);
            if (dumped) continue;
            dumped = true;
            std::wstring path;
            const long code = WriteHangDump(path);
            std::fprintf(stderr, "[brawl-online] hang watch: minidump %s (%s, %ld)\n", Narrow(path).c_str(),
                         code == 0 ? "written" : "failed", code);
            std::fflush(stderr);
            WARN_LOG_FMT(BRAWLBACK, "hang watch: minidump {} ({}, {})", Narrow(path), code == 0 ? "written" : "failed",
                         code);
        }
    }).detach();
#endif
}
namespace {
// Dolphin's "record" session backend (Core/Harness/HarnessOnline.cpp), the same: for tests of the
// game's online menus and the launcher's hand-offs, it records the hand-off and holds the P2P link
// open, so the game stays on its character select with the opponent shown.
class RecordOnlineBackend final : public Online::SessionBackend {
public:
    const char* Name() const override { return "record"; }

    std::optional<std::string> Start(const Online::Match& match, Online::P2PLink link,
                                     const picojson::object& selections) override {
        std::lock_guard lk(m_mutex);
        m_match = match;
        m_selections = selections;
        m_link = std::move(link);
        m_phase = "held";
        ++m_starts;
        NOTICE_LOG_FMT(NETPLAY, "Online session (record): Start match {} ({})", match.match_id,
                       match.is_host ? "host" : "guest");
        return std::nullopt;
    }

    void Stop() override {
        Online::P2PLink link;
        {
            std::lock_guard lk(m_mutex);
            link = std::move(m_link);
            if (!m_phase.empty()) m_phase = "ended";
        }
        if (link.IsOpen()) link.Release(200);
    }

    Online::SessionStatus Status() const override {
        Online::SessionStatus status;
        std::lock_guard lk(m_mutex);
        status.phase = m_phase;
        status.detail["starts"] = picojson::value(static_cast<double>(m_starts));
        status.detail["link_open"] = picojson::value(m_link.IsOpen());
        if (m_match) {
            status.detail["role"] = picojson::value(m_match->is_host ? "host" : "guest");
            status.detail["local_port"] = picojson::value(static_cast<double>(m_match->local_port));
            status.detail["match_id"] = picojson::value(m_match->match_id);
            for (const auto& p : m_match->players) {
                if (!p.is_local) {
                    status.detail["peer_name"] = picojson::value(p.display_name);
                    status.detail["peer_code"] = picojson::value(p.connect_code);
                }
            }
            status.detail["selections"] = picojson::value(m_selections);
        }
        return status;
    }

private:
    mutable std::mutex m_mutex;
    std::optional<Online::Match> m_match;
    picojson::object m_selections;
    Online::P2PLink m_link;
    std::string m_phase;
    int m_starts = 0;
};
}  // namespace

void CoreHarnessStart() {
    const char* port = std::getenv("BRAWL_ONLINE_HARNESS_PORT");
    if (!port || !*port) return;
    Online::Session::RegisterFactory("record", [] { return std::make_unique<RecordOnlineBackend>(); });
    std::thread(Serve, uint16_t(std::atoi(port))).detach();
}

bool CoreHarnessActive() {
    static const bool on = std::getenv("BRAWL_ONLINE_HARNESS_PORT") != nullptr;
    return on;
}

// Every poll of the local controllers (one per frame): the harness's pads replace them.
void CoreHarnessPads(Pad* pads) {
    if (!CoreHarnessActive()) return;
    {
        std::lock_guard lock(s_mutex);
        const uint64_t poll = ++s_polls;
        const auto now = std::chrono::steady_clock::now();
        for (int port = 0; port < 4; ++port) pads[port] = PortPad(port, poll, now);
    }
    s_cv.notify_all();
}

// A look between polls (input suggestions, the core's read after a rollback): the pads as now, no poll counted (a
// script that steps with the polls shows the last poll's entry).
void CoreHarnessPeekPads(Pad* pads) {
    if (!CoreHarnessActive()) return;
    std::lock_guard lock(s_mutex);
    const uint64_t poll = s_polls.load();
    const auto now = std::chrono::steady_clock::now();
    for (int port = 0; port < 4; ++port) pads[port] = PortPad(port, poll, now);
}

}  // namespace BrawlOnlineBridge
