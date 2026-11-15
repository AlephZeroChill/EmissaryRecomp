// Brawl Online's core in-process, the runtime side (brawl_online_bridge.h): guest memory, the CPU at
// the main loop's hooks, the deterministic clock and the pads for the core, and the loop driver.

#include "brawl_online_core.h"
#include "brawl_online_bridge.h"

#include "abi_bridge.h"
#include "brawl_dirty_memory.h"
#include "brawl_snapshot_store.h"
#include "brawl_write_watch.h"
#include "det_clock.h"
#include "frame_input.h"
#include "memory.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <map>
#include <memory>
#include <stdexcept>
#include <vector>
#include <mutex>
#include <string>

void PAD_HLE_SampleLocalPads(FrameInput::GcPad* pads);

namespace {

thread_local CpuContext* t_ctx = nullptr;
std::mutex s_regs_mutex;
std::map<const void*, CpuContext> s_saved_regs;

struct CtxScope {
    CpuContext* previous;
    explicit CtxScope(CpuContext* ctx) : previous(t_ctx) { t_ctx = ctx; }
    ~CtxScope() { t_ctx = previous; }
};

CpuContext& Ctx() {
    if (!t_ctx) {
        if (g_currentCpuContext) return *g_currentCpuContext;
        std::fputs("[brawl-online] core hook without a CPU context\n", stderr);
        std::abort();
    }
    return *t_ctx;
}

// The core's snapshots: the region set packed in physical order, its delta store and write tracking.
struct Snapshots {
    struct Backing { size_t offset, end; uint8_t* data; };
    std::vector<Backing> backing;
    size_t total = 0;
    std::unique_ptr<BrawlRollback::SnapshotStore> store;
    std::unique_ptr<BrawlRollback::WriteWatch> watch;
    int anchor = 0;

    template <bool Write> void Transfer(size_t& cursor, size_t offset, uint8_t* bytes, size_t count) {
        auto at = backing.begin() + cursor;
        if (offset < at->offset || offset >= at->end)
            at = std::lower_bound(backing.begin(), backing.end(), offset,
                                  [](const Backing& r, size_t o) { return r.end <= o; });
        while (count) {
            if (at == backing.end() || offset < at->offset) throw std::runtime_error("snapshot outside packed memory");
            const size_t n = std::min(count, at->end - offset);
            uint8_t* guest = at->data + (offset - at->offset);
            if constexpr (Write) std::memcpy(guest, bytes, n); else std::memcpy(bytes, guest, n);
            bytes += n; offset += n; count -= n;
            if (count) ++at;
        }
        cursor = size_t(at - backing.begin());
    }
};
Snapshots s_snap;

}  // namespace

namespace BrawlOnlineBridge {

void SnapshotRegions(const uint32_t* addrs, const uint32_t* sizes, size_t count) {
    s_snap.watch.reset();
    s_snap.store.reset();
    s_snap.backing.clear();
    s_snap.total = 0;
    s_snap.anchor = 0;
    if (!count) return;
    s_snap.store = std::make_unique<BrawlRollback::SnapshotStore>();
    s_snap.watch = std::make_unique<BrawlRollback::WriteWatch>(reinterpret_cast<uintptr_t>(MKW_FLAT_GUEST_BASE));
    BrawlDirtyMemory::hosts.push_back({reinterpret_cast<uintptr_t>(Memory::GetPointer(Memory::kMem1CachedBase, Memory::kMem1Size)), Memory::kMem1Size, Memory::kMem1CachedBase});
    BrawlDirtyMemory::hosts.push_back({reinterpret_cast<uintptr_t>(Memory::GetPointer(Memory::kMem2CachedBase, Memory::kMem2Size)), Memory::kMem2Size, Memory::kMem2CachedBase});
    for (size_t i = 0; i < count; ++i) {
        uint8_t* host = Memory::GetPointer(addrs[i], sizes[i]);
        s_snap.backing.push_back({s_snap.total, s_snap.total + sizes[i], host});
        s_snap.watch->Add(host, sizes[i], s_snap.total, addrs[i]);
        s_snap.total += sizes[i];
    }
}

void SnapshotSave(int key) {
    auto& s = s_snap;
    if (!s.store) return;
    auto read = [cursor = size_t{0}](size_t offset, uint8_t* out, size_t n) mutable { s_snap.Transfer<false>(cursor, offset, out, n); };
    const std::vector<uint32_t> none;
    const auto& dirty = !s.store->Empty() ? s.watch->Capture(s.total, s.store->Jobs()) : none;
    s.store->Save(key, s.total, read, s.store->Empty() ? nullptr : &dirty, s.anchor);
    s.watch->Enable();
    s.anchor = key;
}

void SnapshotRestore(int key) {
    auto& s = s_snap;
    if (!s.store) return;
    const auto& dirty = s.watch->Capture(s.total, s.store->Jobs());
    auto read = [cursor = size_t{0}](size_t offset, uint8_t* out, size_t n) mutable { s_snap.Transfer<false>(cursor, offset, out, n); };
    auto write = [cursor = size_t{0}](size_t offset, const uint8_t* in, size_t n) mutable { s_snap.Transfer<true>(cursor, offset, const_cast<uint8_t*>(in), n); };
    s.store->Restore(key, s.anchor, dirty, read, write);
    s.watch->Enable();
    s.anchor = key;
}

uint8_t* GuestPointer(uint32_t addr, uint32_t size) {
    return Memory::Contains(addr, size) ? Memory::GetPointer(addr, size) : nullptr;
}
uint8_t* Mem1() { return Memory::GetPointer(Memory::kMem1CachedBase, Memory::kMem1Size); }
uint32_t Mem1Size() { return uint32_t(Memory::kMem1Size); }
uint8_t* Mem2() { return Memory::GetPointer(Memory::kMem2CachedBase, Memory::kMem2Size); }
uint32_t Mem2Size() { return uint32_t(Memory::kMem2Size); }
void MarkDirty(uint32_t addr, uint32_t size) { BrawlDirtyMemory::Mark(addr, size); }

uint32_t Gpr(int index) { return Ctx().gpr[index]; }
void SetGpr(int index, uint32_t value) { Ctx().gpr[index] = value; }
void SetLr(uint32_t value) { Ctx().lr = value; }
void SaveRegs(const void* key) {
    std::lock_guard lock(s_regs_mutex);
    s_saved_regs[key] = Ctx();
}
void RestoreRegs(const void* key) {
    std::lock_guard lock(s_regs_mutex);
    const auto it = s_saved_regs.find(key);
    if (it != s_saved_regs.end()) Ctx() = it->second;
}

uint64_t TimeBase() { return DetClock::Now(); }
void SetResimulating(bool active) { DetClock::SetRollbackResimulating(active); }
void ResetPacing() { DetClock::ResetPacing(); }

Pad LocalPad(int chan) {
    FrameInput::GcPad pads[4]{};
    PAD_HLE_SampleLocalPads(pads);
    const auto& p = pads[chan & 3];
    return {p.connected, p.button, p.stickX, p.stickY, p.substickX, p.substickY, p.triggerL, p.triggerR};
}

void ScreenMessage(const char* text, uint32_t, uint32_t) { std::fprintf(stderr, "[brawl-online] %s\n", text); }
void StopGame() {
    std::fputs("[brawl-online] the core stops the game\n", stderr);
    std::exit(0);
}

}  // namespace BrawlOnlineBridge

namespace BrawlOnlineCore {

namespace B = BrawlOnlineBridge;

bool Enabled() {
    static const bool on = std::getenv("BRAWL_ONLINE_CORE") != nullptr;
    return on;
}

void Start() {
    if (!Enabled()) return;
    DetClock::Enable();
    const char* comm = std::getenv("BRAWL_ONLINE_REPLAY_COMM");
    B::CoreStart(std::getenv("BRAWL_ONLINE_CORE"), comm ? comm : "");
}

void Shutdown() {
    if (Enabled()) B::CoreShutdown();
}

void RunLoop(CpuContext* ctx, Pass pass) {
    CtxScope scope(ctx);
    for (;;) {
        uint32_t call = 0;
        switch (B::CoreLoopTop(&call)) {
        case B::Action::Normal:
            ctx->gpr[25] = 1;
            pass(ctx);
            return;
        case B::Action::Continue:
            pass(ctx);
            return;
        case B::Action::Skip:
            B::CoreLoopEnd();
            return;
        case B::Action::Call:
            // The call returns to the loop top (lr), where the core's hook runs again.
            InvokeIndirectCpu(call, ctx);
            continue;
        }
    }
}

bool FrameEnd(CpuContext* ctx) {
    static bool seen = false;
    if (!seen) { seen = true; std::fprintf(stderr, "[brawl-online] hook reached: frame end 0x80017504\n"); }
    CtxScope scope(ctx);
    return B::CoreFrameEnd();
}

void LoopEnd(CpuContext* ctx) {
    static bool seen = false;
    if (!seen) { seen = true; std::fprintf(stderr, "[brawl-online] hook reached: loop end 0x80017508\n"); }
    CtxScope scope(ctx);
    B::CoreLoopEnd();
}

bool DrivesLoop() { return B::CoreDrivesLoop(); }

void StageCreate(CpuContext* ctx) {
    CtxScope scope(ctx);
    B::CoreStageCreate();
}
void StageFile(CpuContext* ctx) {
    CtxScope scope(ctx);
    B::CoreStageFile();
}
void PadRead(CpuContext* ctx) {
    static bool seen = false;
    if (!seen) { seen = true; std::fprintf(stderr, "[brawl-online] hook reached: pad read after updateLow\n"); }
    CtxScope scope(ctx);
    B::CorePadRead();
}
void HarnessPads(void* gc_pads) {
    if (!Enabled() || !B::CoreHarnessActive()) return;
    auto* pads = static_cast<FrameInput::GcPad*>(gc_pads);
    B::Pad p[4]{};
    B::CoreHarnessPads(p);
    for (int i = 0; i < 4; ++i)
        pads[i] = {p[i].connected, p[i].button, p[i].stickX, p[i].stickY, p[i].substickX, p[i].substickY, p[i].triggerL, p[i].triggerR};
}
void PadThreadUpdated(CpuContext* ctx) {
    static bool seen = false;
    if (!seen) { seen = true; std::fprintf(stderr, "[brawl-online] hook reached: pad thread loop 0x8002BA84\n"); }
    CtxScope scope(ctx);
    B::CorePadThreadUpdated();
}

}  // namespace BrawlOnlineCore
