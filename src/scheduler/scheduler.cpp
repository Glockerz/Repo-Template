#include "scheduler/scheduler.h"
#include "scheduler/rendezvous.h"
#include "lua/state.h"
#include "lua/threads.h"
#include "common/log.h"
#include "common/seh.h"

#include <windows.h>
#include <atomic>

namespace phetamine::sched {
namespace {

// Multi-producer/single-consumer ring. Producers (IPC, watchdog, bootstrap) only
// reserve a tail slot with a CAS; the drain owns the head. POD-only, so no
// allocation ever happens on the producer side or in the drain.
struct Ring {
    Job slots[kQueueCapacity]{};
    std::atomic<uint32_t> head{ 0 };
    std::atomic<uint32_t> tail{ 0 };
    std::atomic<uint32_t> dropped{ 0 };
};

Ring g_ring;
std::atomic<uint64_t> g_nextId{ 1 };
std::atomic<uint64_t> g_processed{ 0 };
std::atomic<uint32_t> g_drainCount{ 0 };
std::atomic<uint64_t> g_lastDrainTick{ 0 };
std::atomic<bool> g_running{ false };
lua_State* g_mainState = nullptr;

struct Parked {
    lua_State* T = nullptr;
    double resumeAt = 0.0;
};
Parked g_parked[kMaxParkedThreads]{};
size_t g_parkedCount = 0;   // main thread writes; Stop clears

double NowSeconds() {
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart) / static_cast<double>(freq.QuadPart);
}

double NowMillis() { return NowSeconds() * 1000.0; }

}  // namespace

void SetMainState(lua_State* L) { g_mainState = L; }
lua_State* MainState() { return g_mainState; }

uint64_t Enqueue(const Job& job) {
    Job copy = job;
    copy.id = g_nextId.fetch_add(1, std::memory_order_relaxed);

    uint32_t tail = g_ring.tail.load(std::memory_order_relaxed);
    for (;;) {
        const uint32_t head = g_ring.head.load(std::memory_order_acquire);
        if (tail - head >= kQueueCapacity) {
            g_ring.dropped.fetch_add(1, std::memory_order_relaxed);
            log::Warn("sched: queue full (head=%u tail=%u) — job %llu dropped",
                      head, tail, static_cast<unsigned long long>(copy.id));
            return 0;
        }
        if (g_ring.tail.compare_exchange_weak(tail, tail + 1, std::memory_order_acq_rel)) break;
    }
    g_ring.slots[tail & (kQueueCapacity - 1)] = copy;
    return copy.id;
}

void ParkYielded(lua_State* T, double resumeAtSeconds) {
    if (!T) return;
    if (g_parkedCount >= kMaxParkedThreads) {
        log::Warn("sched: %zu threads parked already — refusing to park another", g_parkedCount);
        return;
    }
    g_parked[g_parkedCount].T = T;
    g_parked[g_parkedCount].resumeAt = resumeAtSeconds;
    g_parkedCount++;
}

void RetireYielded(lua_State* T) {
    for (size_t i = 0; i < g_parkedCount; i++) {
        if (g_parked[i].T != T) continue;
        g_parked[i] = g_parked[g_parkedCount - 1];
        g_parkedCount--;
        return;
    }
}

size_t ParkedCount() { return g_parkedCount; }

void RetireAllParked() { g_parkedCount = 0; }

int ResumeDue() {
    if (g_parkedCount == 0) return 0;
    lua::Api& api = lua::GetApi();
    if (!api.resume) return 0;

    const double now = NowSeconds();
    const double deadline = NowMillis() + kFrameBudgetMs;
    int resumed = 0;

    for (size_t i = 0; i < g_parkedCount && resumed < kMaxResumesPerFrame; ) {
        if (g_parked[i].resumeAt > now) { i++; continue; }
        if (NowMillis() >= deadline) break;      // budget spent; the rest wait a frame

        lua_State* T = g_parked[i].T;
        g_parked[i] = g_parked[g_parkedCount - 1];
        g_parkedCount--;

        int status = lua::kErrRun;
        __try {
            status = api.resume(T, g_mainState, 0);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            status = lua::kErrRun;
            log::Error("sched: faulted resuming parked thread %p — retiring it", (void*)T);
        }

        if (status == lua::kYield) {
            // Still yielding: re-park for the next frame. The executor converts
            // the yield's requested delay when it can read it.
            ParkYielded(T, NowSeconds() + 0.016);
            resumed++;
            continue;
        }
        if (status != lua::kOk) {
            const char* err = nullptr;
            __try {
                if (api.tostring) err = api.tostring(T, -1);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                err = nullptr;
            }
            jobs::ReportScriptError(T, err ? err : "(error with no message)");
        }
        if (api.resetthread) {
            __try { api.resetthread(T); } __except (EXCEPTION_EXECUTE_HANDLER) { /* best effort */ }
        }
        lua::threads::Remove(T);
        resumed++;
    }
    return resumed;
}

int Drain() {
    g_drainCount.fetch_add(1, std::memory_order_relaxed);
    g_lastDrainTick.store(GetTickCount64(), std::memory_order_relaxed);

    int executed = 0;
    for (;;) {
        const uint32_t head = g_ring.head.load(std::memory_order_relaxed);
        const uint32_t tail = g_ring.tail.load(std::memory_order_acquire);
        if (head == tail) break;

        const Job job = g_ring.slots[head & (kQueueCapacity - 1)];
        g_ring.head.store(head + 1, std::memory_order_release);
        executed++;

        switch (job.kind) {
            case JobKind::RunScript:  jobs::RunScript(job);  break;
            case JobKind::SetIdentity:jobs::SetIdentity(job);break;
            case JobKind::Rebind:     jobs::Rebind(job);     break;
            case JobKind::StopScripts:jobs::StopScripts(job);break;
            case JobKind::CallInto: {
                using call_fn = void (*)(void*);
                if (job.data) {
                    __try {
                        reinterpret_cast<call_fn>(job.data)(reinterpret_cast<void*>(job.arg));
                    } __except (EXCEPTION_EXECUTE_HANDLER) {
                        log::Error("sched: CallInto %p faulted", job.data);
                    }
                }
                break;
            }
            case JobKind::Shutdown:
                g_running.store(false, std::memory_order_relaxed);
                break;
            default:
                break;
        }
        g_processed.fetch_add(1, std::memory_order_relaxed);
        if (executed >= 64) break;   // one frame never drains an unbounded backlog
    }

    ResumeDue();
    return executed;
}

bool Start() {
    if (g_running.load()) return true;
    const rendezvous::Strategy s = rendezvous::Install();
    if (s == rendezvous::Strategy::None) {
        log::StageError("Scheduler",
                        "no main-thread rendezvous could be installed — scripts cannot run "
                        "(docs/OFFSETS.md §4, scheduler/rendezvous.cpp)");
        return false;
    }
    g_running.store(true);
    log::Info("sched: drain driver = %s", rendezvous::Name(s));
    return true;
}

void Stop() {
    g_running.store(false);
    rendezvous::Uninstall();
    RetireAllParked();
}

bool HeartbeatFresh() {
    const uint64_t last = g_lastDrainTick.load();
    if (last == 0) return false;
    return (GetTickCount64() - last) < 2000;
}

uint32_t DrainCount() { return g_drainCount.load(); }
uint64_t Processed() { return g_processed.load(); }
uint32_t Dropped() { return g_ring.dropped.load(); }

}  // namespace phetamine::sched
