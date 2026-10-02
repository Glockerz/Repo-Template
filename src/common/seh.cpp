#include "common/seh.h"

#include <atomic>
#include <cstdio>

namespace phetamine::seh {
namespace {

std::atomic<uintptr_t> g_ownBase{ 0 };
std::atomic<uintptr_t> g_ownEnd{ 0 };
std::atomic<uint32_t> g_faults{ 0 };
std::atomic<uint32_t> g_unexplained{ 0 };
std::atomic<bool> g_backstop{ false };
PVOID g_vehHandle = nullptr;

// Faults above this count are treated as "this session is not trustworthy":
// continuing to run with a half-broken module risks the user's process in a way
// that a clean unload does not (docs/PHETAMINE.md §8).
constexpr uint32_t kUnloadThreshold = 25;

LONG CALLBACK VehHandler(PEXCEPTION_POINTERS info) {
    if (!info || !info->ExceptionRecord || !info->ContextRecord) return EXCEPTION_CONTINUE_SEARCH;
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    // "Was the *instruction* ours?" — not "was the accessed address ours". A
    // wild pointer in our code is a bug we want attributed; an access violation
    // raised by the engine is the engine's business.
    const uintptr_t rip = static_cast<uintptr_t>(info->ContextRecord->Rip);
    if (!IsOwnCode(rip)) return EXCEPTION_CONTINUE_SEARCH;

    const uint32_t count = ++g_faults;
    if (count == 1 || count == kUnloadThreshold) {
        // A VEH runs in an arbitrary context, so this goes to the debugger output
        // only: taking a lock or formatting through the IPC ring here could
        // deadlock the very thread that is already broken.
        char message[160];
        std::snprintf(message, sizeof(message),
                      "PHETAMINE VEH: access violation in our own code (count %u, rip 0x%llX)\n",
                      count, static_cast<unsigned long long>(rip));
        ::OutputDebugStringA(message);
        if (count == kUnloadThreshold) {
            g_unexplained.store(1);
            ::OutputDebugStringA("PHETAMINE VEH: threshold reached — the watchdog will unload\n");
        }
    }
    // Never swallow it: CONTINUE_EXECUTION on a persistent fault would spin the
    // instruction forever, which is worse than the crash the engine would report.
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void SetOwnImageRange(uintptr_t base, size_t size) {
    g_ownBase.store(base, std::memory_order_relaxed);
    g_ownEnd.store(base + size, std::memory_order_relaxed);
}

bool IsOwnCode(uintptr_t addr) {
    const uintptr_t base = g_ownBase.load(std::memory_order_relaxed);
    const uintptr_t end = g_ownEnd.load(std::memory_order_relaxed);
    return base && addr >= base && addr < end;
}

bool IsReadable(uintptr_t addr, size_t n) {
    if (addr < 0x10000 || n == 0) return false;
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &info, sizeof(info)) == 0) return false;
    if (info.State != MEM_COMMIT) return false;
    if (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
    // Do not chase across a region boundary: a readable page followed by an
    // unmapped one is exactly the walk that crashes processes.
    if (addr + n > regionEnd) return false;
    return true;
}

bool IsExecutable(uintptr_t addr, size_t n) {
    if (addr < 0x10000 || n == 0) return false;
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &info, sizeof(info)) == 0) return false;
    if (info.State != MEM_COMMIT) return false;
    const DWORD executable = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                             PAGE_EXECUTE_WRITECOPY;
    return (info.Protect & executable) != 0;
}

bool IsWritable(uintptr_t addr, size_t n) {
    if (addr < 0x10000 || n == 0) return false;
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &info, sizeof(info)) == 0) return false;
    if (info.State != MEM_COMMIT) return false;
    const DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE |
                           PAGE_EXECUTE_WRITECOPY;
    return (info.Protect & writable) != 0;
}

bool InstallBackstop() {
    if (g_backstop.load()) return true;
    g_vehHandle = AddVectoredExceptionHandler(0 /* first */, VehHandler);
    if (!g_vehHandle) return false;
    g_backstop.store(true);
    return true;
}

uint32_t FaultsFromOwnCode() { return g_faults.load(); }

bool ShouldUnloadOnFaults() {
    // The same threshold the VEH uses to mark the session untrustworthy; the
    // watchdog consults this on every tick.
    return g_faults.load() >= kUnloadThreshold || g_unexplained.load() != 0;
}

}  // namespace phetamine::seh
