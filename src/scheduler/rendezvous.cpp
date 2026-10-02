#include "scheduler/rendezvous.h"
#include "scheduler/scheduler.h"
#include "lua/state.h"
#include "lua/env.h"
#include "memory/offsets.h"
#include "common/log.h"
#include "common/seh.h"

#include <windows.h>
#include <tlhelp32.h>
#include <atomic>

namespace phetamine::sched::rendezvous {
namespace {

std::atomic<Strategy> g_strategy{ Strategy::None };
std::atomic<uint32_t> g_apcHits{ 0 };
std::atomic<bool> g_apcArmed{ false };

// Connection sentinel stored in the PHETAMINE.internal table by the bootstrap;
// keeping the reference is what stops the connection from being collected.
int g_connectionRef = -1;

void CALLBACK ApcProbeRoutine(ULONG_PTR) {
    g_apcHits.fetch_add(1, std::memory_order_relaxed);
    log::Info("rendezvous: APC probe delivered — this thread reaches alertable waits "
              "(mechanism viable, still not used for VM work by default)");
}

uint32_t PickRenderThread() {
    const DWORD pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    uint32_t best = 0;
    ULONGLONG bestTime = 0;
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid) continue;
            HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
            if (!h) continue;
            FILETIME creation{}, exit{}, kernel{}, user{};
            if (GetThreadTimes(h, &creation, &exit, &kernel, &user)) {
                const ULONGLONG busy = (static_cast<ULONGLONG>(kernel.dwHighDateTime) << 32 | kernel.dwLowDateTime) +
                                       (static_cast<ULONGLONG>(user.dwHighDateTime) << 32 | user.dwLowDateTime);
                if (busy > bestTime) { bestTime = busy; best = te.th32ThreadID; }
            }
            CloseHandle(h);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    return best;
}

}  // namespace

const char* Name(Strategy s) {
    switch (s) {
        case Strategy::Heartbeat: return "RunService.Heartbeat (Lua-level, engine-driven)";
        case Strategy::Apc:       return "QueueUserAPC (experimental, off by default)";
        case Strategy::TaskQueue: return "ScriptContext task queue (resolved address)";
        case Strategy::FrameSite: return "task-scheduler frame site (resolved address)";
        default:                  return "none";
    }
}

Strategy Current() { return g_strategy.load(); }
bool Armed() { return g_strategy.load() != Strategy::None; }

int HeartbeatCallback(lua_State* L) {
    (void)L;
    // The one and only per-frame entry point. Everything here must be cheap:
    // Drain() is bounded, non-blocking and exception-free.
    __try {
        Drain();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        log::Error("rendezvous: drain faulted — scheduler will be marked unhealthy");
    }
    return 0;
}

Strategy InstallOnMainThread(lua_State* L, const lua::env::Environment& env) {
    lua::Api& api = lua::GetApi();
    if (!L || !env.valid() || !api.IsUsable()) return Strategy::None;

    // The bootstrap runs a tiny chunk that connects our C closure. Using Lua
    // here is deliberate: signal connections are engine-owned, garbage-collected
    // and legal to create from a script context — nothing is patched.
    //
    //   local rs = game:GetService("RunService")
    //   local internal = PHETAMINE.internal
    //   internal.connection = rs.Heartbeat:Connect(internal.heartbeat)
    //
    // PHETAMINE.internal is registered by native_api/misc.cpp (not exposed to
    // scripts: it lives on the closure's upvalue table, not on genv).
    static constexpr char kChunk[] =
        "local ok, err = pcall(function()\n"
        "  local internal = PHETAMINE and PHETAMINE.internal\n"
        "  if not internal then return false, 'no PHETAMINE.internal' end\n"
        "  local rs = game:GetService('RunService')\n"
        "  internal.connection = rs.Heartbeat:Connect(internal.heartbeat)\n"
        "  internal.postsim = rs.PostSimulation:Connect(internal.heartbeat)\n"
        "  return true\n"
        "end)\n"
        "return ok, err\n";

    const int top = api.gettop(L);
    char* bytecode = nullptr;
    __try {
        // The bootstrap chunk runs on the MAIN state, loaded with genv as its
        // environment (so `PHETAMINE.internal` resolves). A new thread is not
        // needed: this code is already running on the client main thread, and
        // running it in a protected call keeps the main state's stack balanced.
        if (lua::env::PushGenv(L, env)) {
            const int envIndex = api.gettop(L);         // [genv]
            size_t bytecodeSize = 0;
            if (api.compile) bytecode = api.compile(kChunk, sizeof(kChunk) - 1, nullptr, &bytecodeSize);
            if (!bytecode || bytecodeSize == 0) {
                log::Error("rendezvous: compiling the bootstrap chunk failed");
            } else if (api.load(L, "=PHETAMINE_BOOTSTRAP", bytecode, bytecodeSize, envIndex) != lua::kOk) {
                const char* err = api.tostring ? api.tostring(L, -1) : nullptr;
                log::Error("rendezvous: the bootstrap chunk did not load: %s", err ? err : "(no message)");
                api.pop(L, 1);
            } else if (api.pcall(L, 0, 2, 0) == lua::kOk) {
                // the chunk returns ok, err — both on the stack now
                ok = api.toboolean ? (api.toboolean(L, -2) != 0) : true;
                if (!ok && api.tostring) {
                    const char* err = api.tostring(L, -1);
                    log::Error("rendezvous: connecting RunService.Heartbeat failed: %s",
                               err ? err : "(no message)");
                }
                api.pop(L, 2);
            } else {
                const char* err = api.tostring ? api.tostring(L, -1) : nullptr;
                log::Error("rendezvous: the bootstrap chunk errored: %s", err ? err : "(no message)");
                api.pop(L, 1);
            }
        } else {
            log::Error("rendezvous: genv could not be pushed — no environment to run the bootstrap in");
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
        log::Error("rendezvous: the bootstrap chunk faulted");
    }
    // The compiled buffer is the client's allocation: it is released on every
    // path, including the faulted one, or not at all (ADR-2).
    if (bytecode && api.free_buf) api.free_buf(bytecode);
    __try { api.settop(L, top); } __except (EXCEPTION_EXECUTE_HANDLER) { /* best effort */ }

    if (!ok) return Strategy::None;
    g_strategy.store(Strategy::Heartbeat);
    log::Info("rendezvous: armed — %s", Name(Strategy::Heartbeat));
    return Strategy::Heartbeat;
}

bool TryArmApcProbe() {
    const uint32_t tid = PickRenderThread();
    if (!tid) {
        log::Warn("rendezvous: no candidate thread for the APC probe");
        return false;
    }
    HANDLE thread = OpenThread(THREAD_SET_CONTEXT, FALSE, tid);
    if (!thread) {
        log::Warn("rendezvous: OpenThread(%u) failed (%lu)", tid, GetLastError());
        return false;
    }
    const bool queued = QueueUserAPC(&ApcProbeRoutine, thread, 0) != 0;
    CloseHandle(thread);
    g_apcArmed.store(queued);
    log::Info("rendezvous: APC probe %s on thread %u (delivery requires an alertable wait)",
              queued ? "queued" : "failed", tid);
    return queued;
}

uint32_t ApcProbeHits() { return g_apcHits.load(); }

Strategy Install() {
    const Strategy armed = g_strategy.load();
    if (armed != Strategy::None) return armed;

    // The reserved strategies (a ScriptContext task-queue insert, a per-frame
    // call site) both need a per-build address AND a call from a context that is
    // allowed to enter the VM. This function runs on the init worker, so it can
    // neither call them nor pretend to: a cached address is reported and left
    // alone until it can be installed from the bootstrap (docs/DECISIONS.md
    // ADR-5). Reporting `Armed()` here without a driver would be a lie that makes
    // EXECUTE hang instead of answering ERROR:Scheduler.
    uintptr_t site = 0;
    if (off::CacheGet("sched.rendezvous.site", site) && site) {
        log::Warn("rendezvous: a frame site is cached for this build (%p) but is not installable "
                  "from a worker thread; wire it in the bootstrap like Heartbeat", (void*)site);
    }

    if (const bool allowApc = false /* PHETAMINE_ALLOW_APC; see DECISIONS ADR-5 */) {
        (void)allowApc;
        if (TryArmApcProbe()) {
            g_strategy.store(Strategy::Apc);
            return Strategy::Apc;
        }
    }

    log::Warn("rendezvous: no strategy armed yet — the bootstrap must run on the client main "
              "thread to connect the Heartbeat drain (the bootstrap in core::BootstrapOnMainThread)");
    return Strategy::None;
}

void Uninstall() {
    g_strategy.store(Strategy::None);
    g_apcArmed.store(false);
    g_connectionRef = -1;
}

}  // namespace phetamine::sched::rendezvous
