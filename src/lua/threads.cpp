#include "lua/threads.h"

#include <windows.h>
#include <cstddef>
#include <vector>

namespace phetamine::lua::threads {
namespace {

SRWLOCK g_lock = SRWLOCK_INIT;
std::vector<lua_State*> g_threads;

// Cheap bloom-ish filter so the hot path (checkcaller from a script) does not
// take the lock in the common case of "not ours": a 64-bit mask of pointer
// bits. False positives are fine (we then take the lock and confirm).
uint64_t g_bloom = 0;
inline uint64_t Bit(lua_State* T) {
    const uint64_t v = reinterpret_cast<uint64_t>(T) >> 4;
    return 1ull << ((v ^ (v >> 6) ^ (v >> 12)) & 63);
}

}  // namespace

void Add(lua_State* T) {
    if (!T) return;
    AcquireSRWLockExclusive(&g_lock);
    for (auto* existing : g_threads) {
        if (existing == T) { ReleaseSRWLockExclusive(&g_lock); return; }
    }
    g_threads.push_back(T);
    g_bloom |= Bit(T);
    ReleaseSRWLockExclusive(&g_lock);
}

void Remove(lua_State* T) {
    if (!T) return;
    AcquireSRWLockExclusive(&g_lock);
    for (size_t i = 0; i < g_threads.size(); i++) {
        if (g_threads[i] == T) {
            g_threads[i] = g_threads.back();
            g_threads.pop_back();
            break;
        }
    }
    // rebuild the mask: it is small and Remove is rare (once per retired script)
    g_bloom = 0;
    for (auto* t : g_threads) g_bloom |= Bit(t);
    ReleaseSRWLockExclusive(&g_lock);
}

bool IsOurs(lua_State* T) {
    if (!T) return false;
    if ((g_bloom & Bit(T)) == 0) return false;   // definitely not ours
    AcquireSRWLockShared(&g_lock);
    bool found = false;
    for (auto* t : g_threads) {
        if (t == T) { found = true; break; }
    }
    ReleaseSRWLockShared(&g_lock);
    return found;
}

bool IsOursCurrent() {
    // The "current thread" question can only be answered by the VM. A C closure
    // knows its own L; outside a script, executor::CurrentScriptThread() reports
    // the thread being drained on the main thread (or nullptr).
    return IsOurs(lua::CurrentScriptThread());
}

size_t Count() {
    AcquireSRWLockShared(&g_lock);
    const size_t n = g_threads.size();
    ReleaseSRWLockShared(&g_lock);
    return n;
}

size_t Snapshot(lua_State** out, size_t max) {
    if (!out || max == 0) return 0;
    AcquireSRWLockShared(&g_lock);
    const size_t n = g_threads.size() < max ? g_threads.size() : max;
    for (size_t i = 0; i < n; i++) out[i] = g_threads[i];
    ReleaseSRWLockShared(&g_lock);
    return n;
}

void Clear() {
    AcquireSRWLockExclusive(&g_lock);
    g_threads.clear();
    g_bloom = 0;
    ReleaseSRWLockExclusive(&g_lock);
}

}  // namespace phetamine::lua::threads
