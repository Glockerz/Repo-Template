#include "executor/executor.h"
#include "scheduler/scheduler.h"
#include "lua/state.h"
#include "lua/env.h"
#include "lua/identity.h"
#include "lua/threads.h"
#include "ipc/protocol.h"
#include "common/log.h"
#include "common/seh.h"

#include <vector>

namespace phetamine::exec {
namespace {

thread_local lua_State* g_currentScript = nullptr;

// The session environment, set by core once the VM is up. Jobs carry the envRef
// so a rebind cannot mix environments between sessions.
lua::env::Environment g_env{};
int g_identity = static_cast<int>(lua::identity::kElevated);

void PushError(lua_State* T, const char* message) {
    lua::Api& api = lua::GetApi();
    if (api.pushstring) api.pushstring(T, message ? message : "(no message)");
}

}  // namespace

// ---- Bytecode ----------------------------------------------------------------

Bytecode::Bytecode(Bytecode&& other) noexcept
    : data(other.data), size(other.size), owned(other.owned) {
    other.data = nullptr;
    other.size = 0;
    other.owned = false;
}

Bytecode& Bytecode::operator=(Bytecode&& other) noexcept {
    if (this != &other) {
        Reset();
        data = other.data;
        size = other.size;
        owned = other.owned;
        other.data = nullptr;
        other.size = 0;
        other.owned = false;
    }
    return *this;
}

Bytecode::~Bytecode() { Reset(); }

void Bytecode::Reset() {
    if (data && owned) {
        lua::Api& api = lua::GetApi();
        if (api.free_buf) {
            __try { api.free_buf(data); } __except (EXCEPTION_EXECUTE_HANDLER) { /* leak beats crash */ }
        } else {
            log::Error("executor: no client free() bound — leaking %zu bytes rather than "
                       "freeing through the wrong heap", size);
        }
    }
    data = nullptr;
    size = 0;
    owned = false;
}

bool Compile(const char* source, size_t length, Bytecode& out, std::string& error) {
    lua::Api& api = lua::GetApi();
    if (!api.compile) {
        error = "luau_compile is not resolved for this build (docs/OFFSETS.md §4)";
        return false;
    }
    if (!source || length == 0) {
        error = "empty source";
        return false;
    }

    size_t outSize = 0;
    char* buffer = nullptr;
    __try {
        buffer = api.compile(source, length, nullptr, &outSize);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        buffer = nullptr;
    }
    if (!buffer || outSize == 0) {
        error = "compilation failed (the fork's C entry point does not report the reason; "
                "syntax errors land here)";
        return false;
    }
    out.Reset();
    out.data = buffer;
    out.size = outSize;
    out.owned = true;
    return true;
}

void SetEnvironment(const lua::env::Environment& env) { g_env = env; }

uint64_t Run(const std::string& source, std::string& error) {
    Bytecode bc;
    if (!Compile(source.data(), source.size(), bc, error)) return 0;

    sched::Job job{};
    job.kind = sched::JobKind::RunScript;
    job.data = bc.data;
    job.size = bc.size;
    job.ref = g_env.genvRef;
    job.arg = static_cast<uintptr_t>(g_identity);

    const uint64_t id = sched::Enqueue(job);
    if (id == 0) {
        error = "scheduler queue is full";
        return 0;                     // Bytecode's destructor frees the buffer
    }
    bc.owned = false;                 // ownership moved to the drain
    return id;
}

bool LoadStringInto(lua_State* L, const char* source, size_t length, std::string& error) {
    lua::Api& api = lua::GetApi();
    if (!api.IsUsable() || !L) {
        error = "the VM is not usable";
        return false;
    }
    Bytecode bc;
    if (!Compile(source, length, bc, error)) return false;

    // Load into the CALLER's thread: `loadstring` returns a function to the
    // script, it does not run anything.
    int envIndex = 0;
    if (g_env.valid() && lua::env::PushGenv(L, g_env)) {
        envIndex = api.gettop(L);
    }
    const int status = api.load(L, "=PHETAMINE", bc.data, bc.size, envIndex);
    if (status != lua::kOk) {
        const char* msg = api.tostring ? api.tostring(L, -1) : nullptr;
        error = msg ? msg : "luau_load failed";
        api.pop(L, 1);
        return false;
    }
    return true;
}

lua_State* CurrentScriptThread() { return g_currentScript; }
void SetCurrentScriptThread(lua_State* T) { g_currentScript = T; }

void StopAll() {
    lua::Api& api = lua::GetApi();
    lua_State* snapshot[sched::kMaxParkedThreads]{};
    const size_t n = lua::threads::Snapshot(snapshot, sched::kMaxParkedThreads);
    for (size_t i = 0; i < n; i++) {
        lua_State* T = snapshot[i];
        if (!T) continue;
        if (api.resetthread) {
            // The fork's lua_resetthread returns void: call it, then assume
            // nothing — the ref is retired either way (docs/PHETAMINE.md §5).
            __try { api.resetthread(T); } __except (EXCEPTION_EXECUTE_HANDLER) { /* abandoned */ }
        }
        sched::RetireYielded(T);
        lua::threads::Remove(T);
    }
    log::Info("executor: stopped and retired %zu script thread(s)", n);
}

}  // namespace phetamine::exec

// ---- scheduler job handlers ---------------------------------------------------
namespace phetamine::sched::jobs {

void ReportScriptError(lua_State* T, const char* message) {
    if (message) {
        // Straight into the fixed-size IPC ring: bounded, no allocation.
        ipc::LogLine(ipc::Level::Error, message);
    }
    (void)T;
}

void RunScript(const Job& job) {
    lua::Api& api = lua::GetApi();
    if (!api.IsUsable() || !job.data || job.size == 0) {
        ReportScriptError(nullptr, "PHETAMINE: cannot run script (VM unusable or empty bytecode)");
        return;
    }

    lua_State* L = MainState();
    if (!L) {
        ReportScriptError(nullptr, "PHETAMINE: no main state bound");
        return;
    }

    lua_State* T = nullptr;
    __try {
        T = api.newthread(L);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        T = nullptr;
    }
    if (!T) {
        ReportScriptError(nullptr, "PHETAMINE: could not create a script thread");
        return;
    }
    lua::threads::Add(T);

    bool loaded = false;
    __try {
        // env: move genv onto the new thread's stack and use it as the env index.
        // luau_load consumes the env value.
        int envIndex = 0;
        if (job.ref >= 0 && lua::env::PushGenvRef(L, job.ref)) {
            api.xmove(L, T, 1);                  // move genv onto the new thread
            envIndex = api.gettop(T);
        }
        const int status = api.load(T, "=PHETAMINE", static_cast<const char*>(job.data), job.size, envIndex);
        loaded = (status == lua::kOk);
        if (!loaded) {
            const char* msg = api.tostring ? api.tostring(T, -1) : nullptr;
            ReportScriptError(T, msg ? msg : "luau_load failed");
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        loaded = false;
        ReportScriptError(T, "PHETAMINE: fault while loading bytecode (bad luau_load binding?)");
    }

    if (loaded) {
        // Identity is a property of the thread, applied before the first resume.
        if (lua::identity::Resolved() && job.arg) {
            lua::identity::Set(T, job.arg);
        }

        exec::SetCurrentScriptThread(T);
        int status = lua::kErrRun;
        __try {
            status = api.resume(T, L, 0);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            status = lua::kErrRun;
            ReportScriptError(T, "PHETAMINE: fault while resuming the script thread");
        }
        exec::SetCurrentScriptThread(nullptr);

        if (status == lua::kYield) {
            // Parked; resumed by ResumeDue() under the frame budget. The requested
            // delay is read when the yield came from our wait shim (task.wait).
            sched::ParkYielded(T, /*resumeAt*/ 0.0);   // due immediately; the budget paces it
        } else if (status != lua::kOk) {
            const char* msg = nullptr;
            __try {
                if (api.tostring) msg = api.tostring(T, -1);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                msg = nullptr;
            }
            ReportScriptError(T, msg ? msg : "(error with no message)");
            if (api.resetthread) {
                __try { api.resetthread(T); } __except (EXCEPTION_EXECUTE_HANDLER) { /* best effort */ }
            }
            lua::threads::Remove(T);
        } else {
            // Completed normally: nothing to park, retire the thread.
            if (api.resetthread) {
                __try { api.resetthread(T); } __except (EXCEPTION_EXECUTE_HANDLER) { /* best effort */ }
            }
            lua::threads::Remove(T);
        }
    } else {
        if (api.resetthread) {
            __try { api.resetthread(T); } __except (EXCEPTION_EXECUTE_HANDLER) { /* best effort */ }
        }
        lua::threads::Remove(T);
    }

    // The bytecode buffer belongs to the job; release it through the client's
    // allocator now that the chunk has been loaded (Luau copies what it needs).
    exec::Bytecode owner;
    owner.data = static_cast<char*>(job.data);
    owner.size = job.size;
    owner.owned = true;
    owner.Reset();
}

void SetIdentity(const Job& job) {
    (void)job;
    log::Warn("sched: SetIdentity job is not used — identity is applied per script thread");
}

void Rebind(const Job& job) {
    (void)job;
    extern void RebindOnMainThread();
    RebindOnMainThread();
}

void StopScripts(const Job& job) {
    (void)job;
    exec::StopAll();
}

}  // namespace phetamine::sched::jobs
