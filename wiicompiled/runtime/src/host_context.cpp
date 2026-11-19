#include "host_context.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif (defined(__APPLE__) || defined(__linux__) || defined(__SWITCH__)) && defined(__aarch64__)
#if defined(__SWITCH__)
#include <cstdlib>
#else
#include <sys/mman.h>
#endif
#include <unistd.h>
#include <limits>
#if defined(__ANDROID__)
#include <pthread.h>

#include <cstdint>
#include <vector>
#endif

extern "C" void mkw_co_switch(void** targetSp, void** sourceSp);
extern "C" void* mkw_co_init(void* stackTop, void (*entry)(void*), void* argument);
#elif defined(__linux__) || (defined(__APPLE__) && defined(__x86_64__))
#include <libco.h>

#include <cstdlib>
#include <limits>
#include <unordered_map>
#else
#error "HostContext needs a supported cooperative-context backend"
#endif

namespace HostContext {

void* StackPointer()
{
    void* stack;
#if defined(__x86_64__)
    asm volatile("mov %%rsp, %0" : "=r"(stack));
#elif defined(__aarch64__)
    asm volatile("mov %0, sp" : "=r"(stack));
#endif
    return stack;
}

#if defined(_WIN32)

namespace {
thread_local bool g_convertedScheduler = false;
}

bool InitializeScheduler(Handle* scheduler)
{
    void* context = ConvertThreadToFiber(nullptr);
    g_convertedScheduler = context != nullptr;
    if (!context && IsThreadAFiber()) {
        context = GetCurrentFiber();
    }
    *scheduler = context;
    return context != nullptr;
}

void ShutdownScheduler(Handle scheduler)
{
    if (scheduler && g_convertedScheduler) {
        ConvertFiberToThread();
    }
    g_convertedScheduler = false;
}

Handle Create(std::size_t stackSize, Entry entry, void* argument)
{
    return CreateFiber(stackSize, entry, argument);
}

void Destroy(Handle context)
{
    if (context) {
        DeleteFiber(context);
    }
}

Handle Current()
{
    return GetCurrentFiber();
}

bool IsCurrent(Handle context)
{
    return context != nullptr && GetCurrentFiber() == context;
}

void Switch(Handle target)
{
    SwitchToFiber(target);
}

#elif (defined(__APPLE__) || defined(__linux__) || defined(__SWITCH__)) && defined(__aarch64__)

namespace {
struct Context {
    void* savedStackPointer = nullptr;
    void* stack = nullptr;
    std::size_t stackSize = 0;
};

// Guest scheduling is confined to the initialized main host thread. Keeping
// this as ordinary process state also avoids relying on Darwin TLS internals
// while executing on a manually managed stack.
#if defined(__APPLE__) || defined(__ANDROID__)
Context* g_current = nullptr;
#else
thread_local Context* g_current = nullptr;
#endif

#if defined(__ANDROID__)
// ART compares the stack pointer with the Java thread's stack bounds whenever native code calls
// into Java (SDL does, for input and lifecycle events, from inside the guest scheduler) and treats
// anything outside them as a stack overflow. Guest fiber stacks therefore come from the host
// thread's own stack (64 MB, set where SDLActivity creates the thread): the lower half, above a
// generous skip over ART's protected region, while the thread's own frames keep the upper half.
struct StackArena {
    uintptr_t next = 0;   // first never-used address
    uintptr_t limit = 0;  // end of the arena; the thread's own frames live above it
    std::vector<std::pair<void*, std::size_t>> freed;  // stacks of destroyed fibers, for reuse
};
StackArena g_arena;
constexpr std::size_t kArenaSkipBytes = 2u * 1024 * 1024;

bool InitializeStackArena()
{
    g_arena = {};
    pthread_attr_t attr;
    if (pthread_getattr_np(pthread_self(), &attr) != 0) {
        return false;
    }
    void* base = nullptr;
    std::size_t size = 0;
    const int result = pthread_attr_getstack(&attr, &base, &size);
    pthread_attr_destroy(&attr);
    if (result != 0 || base == nullptr || size < 16u * 1024 * 1024) {
        return false;
    }
    const uintptr_t low = reinterpret_cast<uintptr_t>(base);
    g_arena.next = low + kArenaSkipBytes;
    g_arena.limit = low + size / 2;
    return true;
}

void* AllocateArenaStack(std::size_t totalSize)
{
    for (auto it = g_arena.freed.begin(); it != g_arena.freed.end(); ++it) {
        if (it->second == totalSize) {
            void* stack = it->first;
            g_arena.freed.erase(it);
            return stack;
        }
    }
    if (g_arena.next == 0 || totalSize > g_arena.limit - g_arena.next) {
        return nullptr;
    }
    void* stack = reinterpret_cast<void*>(g_arena.next);
    g_arena.next += totalSize;
    return stack;
}
#endif
}

bool InitializeScheduler(Handle* scheduler)
{
#if defined(__ANDROID__)
    if (!InitializeStackArena()) {
        *scheduler = nullptr;
        return false;
    }
#endif
    auto* context = new Context();
    g_current = context;
    *scheduler = context;
    return true;
}

void ShutdownScheduler(Handle scheduler)
{
    auto* context = static_cast<Context*>(scheduler);
    if (g_current == context) {
        g_current = nullptr;
    }
    delete context;
}

Handle Create(std::size_t stackSize, Entry entry, void* argument)
{
#if defined(__SWITCH__)
    // Horizon gives applications no page protection to make a guard page with: heap stacks.
    constexpr std::size_t kPage = 4096;
    if (stackSize == 0 || stackSize > std::numeric_limits<std::size_t>::max() - kPage) {
        return nullptr;
    }
    const std::size_t totalSize = (stackSize + kPage - 1) / kPage * kPage;
    auto* context = new Context();
    context->stack = std::aligned_alloc(kPage, totalSize);
    if (!context->stack) {
        delete context;
        return nullptr;
    }
#else
    const std::size_t guardSize = static_cast<std::size_t>(getpagesize());
    if (stackSize == 0 || stackSize > std::numeric_limits<std::size_t>::max() - 2 * guardSize) {
        return nullptr;
    }
    // Keep each stack and its guard page aligned, including successive Android
    // arena allocations when callers request a non-page-multiple stack size.
    const std::size_t totalSize = ((stackSize + guardSize - 1) / guardSize + 1) * guardSize;
    auto* context = new Context();
#if defined(__ANDROID__)
    context->stack = AllocateArenaStack(totalSize);
    // Separate mappings can violate ART's JNI stack checks.
    if (!context->stack) {
        delete context;
        return nullptr;
    }
#else
    context->stack = mmap(nullptr, totalSize, PROT_READ | PROT_WRITE,
                          MAP_ANON | MAP_PRIVATE, -1, 0);
    if (context->stack == MAP_FAILED) {
        delete context;
        return nullptr;
    }
#endif
    // Fault on stack overflow instead of corrupting the preceding mapping.
    if (mprotect(context->stack, guardSize, PROT_NONE) != 0) {
#if defined(__ANDROID__)
        g_arena.freed.emplace_back(context->stack, totalSize);
#else
        munmap(context->stack, totalSize);
#endif
        delete context;
        return nullptr;
    }
#endif
    context->stackSize = totalSize;

    auto* stackTop = static_cast<char*>(context->stack) + totalSize;
    context->savedStackPointer = mkw_co_init(stackTop, entry, argument);
    return context;
}

void Destroy(Handle context)
{
    auto* nativeContext = static_cast<Context*>(context);
    if (!nativeContext) {
        return;
    }
    if (nativeContext->stack) {
#if defined(__ANDROID__)
        // Arena stacks belong to the host thread, not to a separate mapping.
        if (mprotect(nativeContext->stack, static_cast<std::size_t>(getpagesize()), PROT_READ | PROT_WRITE) == 0) {
            g_arena.freed.emplace_back(nativeContext->stack, nativeContext->stackSize);
        }
#elif defined(__SWITCH__)
        std::free(nativeContext->stack);
#else
        munmap(nativeContext->stack, nativeContext->stackSize);
#endif
    }
    delete nativeContext;
}

Handle Current()
{
    return g_current;
}

bool IsCurrent(Handle context)
{
    return context != nullptr && context == g_current;
}

void Switch(Handle target)
{
    auto* destination = static_cast<Context*>(target);
    Context* source = g_current;
    if (!destination || destination == source) {
        return;
    }

    g_current = destination;
    mkw_co_switch(&destination->savedStackPointer, &source->savedStackPointer);
    g_current = source;
}

#elif defined(__linux__) || (defined(__APPLE__) && defined(__x86_64__))

// ThreadSanitizer builds: libco switches stacks behind TSan's back, which then mixes up the fibers' shadow stacks and
// reports nonsense. Its fiber API keeps it informed (compiled only under -fsanitize=thread).
#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define MKW_TSAN_FIBERS 1
extern "C" void* __tsan_get_current_fiber(void);
extern "C" void* __tsan_create_fiber(unsigned flags);
extern "C" void __tsan_destroy_fiber(void* fiber);
extern "C" void __tsan_switch_to_fiber(void* fiber, unsigned flags);
#endif
#endif
#ifndef MKW_TSAN_FIBERS
#define MKW_TSAN_FIBERS 0
#endif

namespace {
struct Context {
    cothread_t native = nullptr;
    Entry entry = nullptr;
    void* argument = nullptr;
    bool ownsNative = false;
#if MKW_TSAN_FIBERS
    void* tsanFiber = nullptr;
#endif
};

thread_local Context* g_current = nullptr;
thread_local std::unordered_map<cothread_t, Context*> g_contexts;

void ContextEntry()
{
    const auto found = g_contexts.find(co_active());
    if (found == g_contexts.end() || !found->second || !found->second->entry) {
        std::abort();
    }

    Context* context = found->second;
    g_current = context;
    context->entry(context->argument);

    // A guest fiber must return through FiberProc's scheduler handoff. There
    // is no valid native caller to return to from libco's entry trampoline.
    std::abort();
}
} // namespace

bool InitializeScheduler(Handle* scheduler)
{
    auto* context = new Context();
    context->native = co_active();
    if (!context->native) {
        delete context;
        return false;
    }

#if MKW_TSAN_FIBERS
    context->tsanFiber = __tsan_get_current_fiber();
#endif
    g_current = context;
    g_contexts.emplace(context->native, context);
    *scheduler = context;
    return true;
}

void ShutdownScheduler(Handle scheduler)
{
    auto* context = static_cast<Context*>(scheduler);
    if (!context) {
        return;
    }

    g_contexts.erase(context->native);
    if (g_current == context) {
        g_current = nullptr;
    }
    delete context;
}

Handle Create(std::size_t stackSize, Entry entry, void* argument)
{
    if (stackSize > std::numeric_limits<unsigned int>::max()) {
        return nullptr;
    }
    auto* context = new Context();
    context->entry = entry;
    context->argument = argument;
    context->native = co_create(static_cast<unsigned int>(stackSize), ContextEntry);
    context->ownsNative = context->native != nullptr;
    if (!context->native) {
        delete context;
        return nullptr;
    }
#if MKW_TSAN_FIBERS
    context->tsanFiber = __tsan_create_fiber(0);
#endif

    g_contexts.emplace(context->native, context);
    return context;
}

void Destroy(Handle context)
{
    auto* nativeContext = static_cast<Context*>(context);
    if (!nativeContext) {
        return;
    }

    g_contexts.erase(nativeContext->native);
    if (nativeContext->ownsNative) {
        co_delete(nativeContext->native);
#if MKW_TSAN_FIBERS
        __tsan_destroy_fiber(nativeContext->tsanFiber);
#endif
    }
    delete nativeContext;
}

Handle Current()
{
    return g_current;
}

bool IsCurrent(Handle context)
{
    return context != nullptr && context == g_current;
}

void Switch(Handle target)
{
    auto* destination = static_cast<Context*>(target);
    Context* source = g_current;
    if (!destination || destination == source) {
        return;
    }

    g_current = destination;
#if MKW_TSAN_FIBERS
    __tsan_switch_to_fiber(destination->tsanFiber, 0);
#endif
    co_switch(destination->native);
    g_current = source;
}

#endif

} // namespace HostContext
