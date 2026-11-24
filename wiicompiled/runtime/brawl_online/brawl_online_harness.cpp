// Brawl Online's test harness in EmissaryRecomp (core side, brawl_online_bridge.h): the part of
// Dolphin's harness protocol (brawlonline.net harness/ppharness, Core/Harness/HarnessServer.cpp)
// its online tests drive a game with: memory, pads by the game's pad polls, waits, and the core's
// own status. On when BRAWL_ONLINE_HARNESS_PORT is set; one client at a time, loopback only.

#include "brawl_online_bridge.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <dbghelp.h>
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
};
std::array<Port, 4> s_ports;

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
        if (const picojson::value* sv = Find(a, "start"); sv && sv->is<double>()) start = uint64_t(sv->get<double>());
        p.script = std::move(timeline);
        p.script_start = start;
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
        r["active"] = picojson::value(!p.script.empty() && polls < end);
        r["remaining"] = Num(double(!p.script.empty() && polls < end ? end - polls : 0));
        return r;
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
    // Rooms' requests (watch_leave, watch_hold, ...: Rooms::HarnessRequest, as Dolphin's rooms_request).
    if (cmd == "rooms_request") {
        if (const auto error = Online::Rooms::HarnessRequest(a)) throw Error(*error);
        return Online::Rooms::Status();
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
    if (cmd == "replay_recording") {
        if (Bool(a, "flush", false)) Gprb::Replay::Flush();
        Object r;
        r["path"] = picojson::value(Gprb::Replay::CurrentPath());
        r["recording"] = picojson::value(Gprb::Replay::Active());
        return r;
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
    if (cmd == "quit" || cmd == "stop_game") {
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
                for (int depth = 0; depth < 64 && ctx.Rip; ++depth) {
                    DWORD64 image = 0;
                    PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &image, nullptr);
                    const uint64_t start = fn ? image + fn->BeginAddress : ctx.Rip;
                    if (depth == 0) ++counts[start].self;
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

// The game's thread's native stack now, as module+offset (the executable's resolve with WiiCompiled.map):
// for the core's loop watch, when the game's thread has not run a session frame for a second. The thread
// is suspended only while its frames are unwound into a fixed array (nothing allocates meanwhile: it may
// hold the heap's lock), then named after it runs again.
void CoreGameThreadStack(char* out, size_t size) {
    if (size) out[0] = 0;
#ifdef _WIN32
    const HANDLE target = s_gameThread.load();
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
    size_t used = std::snprintf(out, size, "game thread stack");
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
#endif
}

void CoreHarnessStart() {
    const char* port = std::getenv("BRAWL_ONLINE_HARNESS_PORT");
    if (!port || !*port) return;
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
        for (int port = 0; port < 4; ++port) {
            auto& p = s_ports[size_t(port)];
            B::Pad pad{};
            // A controller on port 1 only, as Dolphin's harness instances have (its other ports read
            // "no controller", error 0xFF): the game's pad memory is the same on both.
            pad.connected = port == 0;
            if (p.set) pad = *p.set;
            if (!p.script.empty() && poll >= p.script_start && poll < p.script_start + p.script.size())
                pad = p.script[size_t(poll - p.script_start)];
            pads[port] = pad;
        }
    }
    s_cv.notify_all();
}

}  // namespace BrawlOnlineBridge
