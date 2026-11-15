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
#include <cstring>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
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

#include <picojson.h>

#include "BrawlOnline/Online/GameBridge.h"
#include "BrawlOnline/Online/OnlineClient.h"
#include "BrawlOnline/Online/OnlineSession.h"
#include "BrawlOnline/Online/Rooms.h"
#include "BrawlOnline/Rollback/GameplaySession.h"

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

std::string Hex(const uint8_t* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s(n * 2, '0');
    for (size_t i = 0; i < n; ++i) { s[2 * i] = d[p[i] >> 4]; s[2 * i + 1] = d[p[i] & 15]; }
    return s;
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
    if (cmd == "ping" || cmd == "hello") return {};
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
    if (cmd == "game_bridge_status") return Online::GameBridge::Status();
    if (cmd == "rooms_status") return Online::Rooms::Status();
    if (cmd == "online_status") return Online::Client::OnlineStatus();
    if (cmd == "mm_status") return Online::Client::MatchmakingStatus();
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
    if (cmd == "screenshot") return {};  // the recomp draws its own window; no capture here yet
    if (cmd == "quit" || cmd == "stop_game") {
        std::thread([] { std::this_thread::sleep_for(std::chrono::milliseconds(100)); B::StopGame(); }).detach();
        return {};
    }
    throw Error("unknown command " + cmd);
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
    if (bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(listener, 1) != 0) {
        std::fprintf(stderr, "[brawl-online] harness: cannot listen on 127.0.0.1:%u\n", port);
        return;
    }
    std::fprintf(stderr, "[brawl-online] harness listening on 127.0.0.1:%u\n", port);
    for (;;) {
        const Socket c = accept(listener, nullptr, nullptr);
        if (c == INVALID_SOCKET) continue;
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&yes), sizeof(yes));
        std::string buf;
        char chunk[65536];
        for (;;) {
            const int n = int(recv(c, chunk, sizeof(chunk), 0));
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
                        resp["result"] = picojson::value(Dispatch(o.count("cmd") ? o.at("cmd").to_str() : "", o));
                        resp["ok"] = picojson::value(true);
                    } catch (const std::exception& e) {
                        resp["ok"] = picojson::value(false);
                        resp["error"] = picojson::value(std::string(e.what()));
                    }
                }
                const std::string out = picojson::value(resp).serialize() + "\n";
                send(c, out.data(), int(out.size()), 0);
            }
        }
        closesocket(c);
    }
}

}  // namespace

namespace BrawlOnlineBridge {

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
