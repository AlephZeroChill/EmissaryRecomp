#include "det_hash.h"

#include "axfx_async.h"
#include "brawl_rollback.h"
#include "hle/audio/ax_dsp.h"

#include "det_clock.h"
#include "memory.h"
#include "runtime_config.h"
#include "runtime_log.h"

#include <xxhash.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace DetHash {
namespace {

constexpr uint32_t kMem1Base = 0x80000000u;
constexpr uint32_t kMem2Base = 0x90000000u;
constexpr size_t kMem1Bytes = 24u * 1024u * 1024u;
constexpr size_t kMem2Bytes = 64u * 1024u * 1024u;

struct Settings {
    std::filesystem::path hashFile;
    uint32_t every = 60;
    std::set<uint32_t> dumps;
    std::filesystem::path dumpDirectory;
    // NSMBW_DET_HASH_SKIP: guest ranges left out of the hash (see SkipRanges).
    std::vector<std::pair<uint32_t, uint32_t>> skip;
    bool skipThreads = false;
};

const Settings& GetSettings() {
    static const Settings settings = [] {
        Settings parsed;
        if (const char* hash = std::getenv("NSMBW_DET_HASH"); hash != nullptr && *hash != '\0') {
            std::string text(hash);
            // "<file>:<every>", but a Windows drive letter's colon is part of the path.
            const size_t colon = text.rfind(':');
            if (colon != std::string::npos && colon > 1 && colon + 1 < text.size() &&
                text.find_first_not_of("0123456789", colon + 1) == std::string::npos) {
                parsed.every = static_cast<uint32_t>(std::strtoul(text.c_str() + colon + 1, nullptr, 10));
                text.resize(colon);
            }
            if (parsed.every == 0) {
                parsed.every = 1;
            }
            parsed.hashFile = std::filesystem::u8path(text);
            if (parsed.hashFile.is_relative()) {
                parsed.hashFile = RuntimeConfigFile::ApplicationDataDirectory() / parsed.hashFile;
            }
        }
        if (const char* dump = std::getenv("NSMBW_DET_DUMP"); dump != nullptr && *dump != '\0') {
            for (const char* p = dump; *p != '\0';) {
                char* end = nullptr;
                const unsigned long retrace = std::strtoul(p, &end, 10);
                if (end == p) {
                    break;
                }
                parsed.dumps.insert(static_cast<uint32_t>(retrace));
                p = *end == ',' ? end + 1 : end;
            }
        }
        // "start-end" hex guest ranges (end exclusive) and/or "threads", comma separated.
        if (const char* skip = std::getenv("NSMBW_DET_HASH_SKIP"); skip != nullptr && *skip != '\0') {
            std::string text(skip);
            for (size_t begin = 0; begin < text.size();) {
                size_t end = text.find(',', begin);
                if (end == std::string::npos) end = text.size();
                const std::string token = text.substr(begin, end - begin);
                if (token == "threads") {
                    parsed.skipThreads = true;
                } else if (const size_t dash = token.find('-'); dash != std::string::npos) {
                    parsed.skip.emplace_back(static_cast<uint32_t>(std::strtoul(token.c_str(), nullptr, 16)),
                                             static_cast<uint32_t>(std::strtoul(token.c_str() + dash + 1, nullptr, 16)));
                }
                begin = end + 1;
            }
        }
        parsed.dumpDirectory = parsed.hashFile.empty() ? RuntimeConfigFile::ApplicationDataDirectory() / "Logs"
                                                       : parsed.hashFile.parent_path();
        if (!parsed.hashFile.empty() || !parsed.dumps.empty()) {
            RT_LOGF(RT_TAG_RUNTIME, "determinism diagnostics: hashes every %u retraces to %s, %zu dump(s)\n",
                    parsed.every, RuntimeConfigFile::PathToUtf8(parsed.hashFile).c_str(), parsed.dumps.size());
        }
        return parsed;
    }();
    return settings;
}

// The configured ranges plus, for "threads", every guest thread's stack [stackEnd, stackBase) and saved register
// context (the OSThread's leading OSContext, 0x2C8 bytes), walked from the OS thread list on every hash. Builds that
// keep register values out of CpuContext (MKW_EABI_REGS) leave different bytes in both while game state matches.
std::vector<std::pair<uint32_t, uint32_t>> SkipRanges(const Settings& settings) {
    std::vector<std::pair<uint32_t, uint32_t>> ranges = settings.skip;
    if (settings.skipThreads) {
        uint32_t thread = Memory::Contains(0x800000DCu, 4) ? Memory::Read32(0x800000DCu) : 0;
        for (int count = 0; thread != 0 && count < 256 && Memory::Contains(thread, 0x30Cu); ++count) {
            const uint32_t stackBase = Memory::Read32(thread + 0x304u);
            const uint32_t stackEnd = Memory::Read32(thread + 0x308u);
            if (stackEnd < stackBase && stackBase - stackEnd <= 0x01000000u) {
                ranges.emplace_back(stackEnd, stackBase);
            }
            ranges.emplace_back(thread, thread + 0x2C8u);
            thread = Memory::Read32(thread + 0x2FCu);
        }
    }
    std::sort(ranges.begin(), ranges.end());
    return ranges;
}

uint64_t HashRange(uint32_t base, size_t bytes, const std::vector<std::pair<uint32_t, uint32_t>>& skip = {}) {
    if (!Memory::Contains(base, bytes)) {
        return 0;
    }
    if (skip.empty()) {
        return XXH3_64bits(Memory::GetPointer(base, bytes), bytes);
    }
    XXH3_state_t* state = XXH3_createState();
    XXH3_64bits_reset(state);
    const uint64_t end = static_cast<uint64_t>(base) + bytes;
    uint64_t cursor = base;
    for (const auto& [from, to] : skip) {
        const uint64_t skipFrom = std::max<uint64_t>(from, cursor);
        const uint64_t skipTo = std::min<uint64_t>(to, end);
        if (skipTo <= skipFrom) {
            continue;
        }
        XXH3_64bits_update(state, Memory::GetPointer(static_cast<uint32_t>(cursor), 1), skipFrom - cursor);
        cursor = skipTo;
    }
    XXH3_64bits_update(state, Memory::GetPointer(static_cast<uint32_t>(cursor), 1), end - cursor);
    const uint64_t digest = XXH3_64bits_digest(state);
    XXH3_freeState(state);
    return digest;
}

void WriteImage(const std::filesystem::path& path, uint32_t base, size_t bytes) {
    if (!Memory::Contains(base, bytes)) {
        return;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(Memory::GetPointer(base, bytes)), static_cast<std::streamsize>(bytes));
}

} // namespace

Digest HashGuestMemory() {
    const auto skip = SkipRanges(GetSettings());
    return {HashRange(kMem1Base, kMem1Bytes, skip), HashRange(kMem2Base, kMem2Bytes, skip)};
}

void OnRetrace(uint32_t retraceCount) {
    AxfxAsync::Join();  // the images include the AXFX jobs' buffers and lines (axfx_async.h)
    const Settings& settings = GetSettings();
    if (!settings.hashFile.empty() && retraceCount % settings.every == 0) {
        static std::ofstream file = [&] {
            std::error_code ec;
            std::filesystem::create_directories(settings.hashFile.parent_path(), ec);
            return std::ofstream(settings.hashFile, std::ios::trunc);
        }();
        if (file) {
            // The AX mix worker writes guest audio buffers asynchronously; hashing mid-mix made MEM1 differ between
            // runs of one build (~half the lines). Wait for it, as the netplay hash does (netplay_session.cpp).
            AxDspHle::JoinMixWorker();
            const Digest digest = HashGuestMemory();
            // Fifth column: the netplay gameplay checksum (fighters, RNG, frame counters); its pointer walk can
            // fault on half-built state during boot, which counts as 0.
            uint32_t gameplay = 0;
            try {
                gameplay = BrawlRollback::CurrentGameplayHash();
            } catch (...) {
            }
            char line[112];
            std::snprintf(line, sizeof(line), "%u %llu %016llx %016llx %08x\n", retraceCount,
                          static_cast<unsigned long long>(DetClock::Now()),
                          static_cast<unsigned long long>(digest.mem1), static_cast<unsigned long long>(digest.mem2),
                          gameplay);
            file << line << std::flush;
        }
    }
    if (settings.dumps.count(retraceCount) != 0) {
        AxDspHle::JoinMixWorker();  // (as above: no half-mixed audio buffers in the images)
        std::error_code ec;
        std::filesystem::create_directories(settings.dumpDirectory, ec);
        const std::string stem = "mem_r" + std::to_string(retraceCount);
        WriteImage(settings.dumpDirectory / (stem + "_mem1.bin"), kMem1Base, kMem1Bytes);
        WriteImage(settings.dumpDirectory / (stem + "_mem2.bin"), kMem2Base, kMem2Bytes);
        RT_LOGF(RT_TAG_RUNTIME, "determinism diagnostics: memory images at retrace %u\n", retraceCount);
    }
}

} // namespace DetHash
