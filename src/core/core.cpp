#include "core/core.h"
#include "lua/state.h"
#include "lua/env.h"
#include "lua/identity.h"
#include "lua/threads.h"
#include "scheduler/scheduler.h"
#include "scheduler/rendezvous.h"
#include "executor/executor.h"
#include "native_api/api.h"
#include "memory/offsets.h"
#include "memory/instance_walker.h"
#include "memory/pe.h"
#include "ipc/pipe_server.h"
#include "inject/loader.h"
#include "common/log.h"
#include "common/seh.h"

#include <windows.h>
#include <atomic>
#include <cstdio>
#include <thread>
#include <string>
#include <vector>

namespace phetamine::core {
namespace {

Stage g_stage = Stage::None;
std::atomic<bool> g_running{ false };
std::atomic<bool> g_unload{ false };
std::atomic<bool> g_bootstrapped{ false };

uintptr_t g_clientBase = 0;
uintptr_t g_dataModel = 0;
uintptr_t g_scriptContext = 0;
lua::State g_state{};
lua::env::Environment g_env{};
api::Registrar g_registry{};

SRWLOCK g_teleportLock = SRWLOCK_INIT;
std::vector<std::string> g_teleportQueue;

std::wstring g_workspace;

// The Ready frame is posted from the bootstrap (client main thread) and possibly
// re-posted by the accept thread if the UI connected late — hence a small lock
// rather than a one-shot flag.
SRWLOCK g_infoLock = SRWLOCK_INIT;
ipc::Info g_lastInfo;
std::atomic<bool> g_readyPosted{ false };

void PostReadyStored() {
    AcquireSRWLockShared(&g_infoLock);
    ipc::Info copy = g_lastInfo;
    ReleaseSRWLockShared(&g_infoLock);
    ipc::PostReady(copy);
}

// AcceptOne() blocks in ConnectNamedPipe, so it cannot run on the init thread
// (that thread becomes the watchdog). Started right after ipc::Start().
void AcceptThread() {
    if (!ipc::AcceptOne()) {
        log::Error("core: no UI connected to the pipe; the module stays usable through the log "
                   "ring only");
        return;
    }
    if (g_readyPosted.load()) {
        // The UI connected after the bootstrap finished: the Ready/Canary frames
        // are already in the ring but were sent before the pipe had a client, so
        // they were dropped. Re-post the state snapshot now.
        PostReadyStored();
        char canaryJson[1024];
        RunCanary(canaryJson, sizeof(canaryJson));
        ipc::PostCanary(canaryJson);
    }
}

std::wstring DefaultWorkspace() {
    wchar_t buffer[MAX_PATH]{};
    DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);
    std::wstring root(buffer, length);
    root += L"\\PHETAMINE\\workspace";
    CreateDirectoryW((root + L"\\autoexec").c_str(), nullptr);
    CreateDirectoryW((root + L"\\scripts").c_str(), nullptr);
    return root;
}

void Fail(Stage stage, const char* why) {
    log::StageError(StageName(stage), "%s", why);
    Shutdown();
}

void FillInfo(ipc::Info& info) {
    info.placeId = mem::GetPlaceId(g_dataModel);
    info.gameId = mem::GetGameId(g_dataModel);
    const uintptr_t player = mem::GetLocalPlayer(g_dataModel);
    if (player) {
        const uintptr_t userOffset = off::Get("player.user_id");
        uintptr_t id = 0;
        if (userOffset && seh::TryReadPtr(player + userOffset, id)) info.userId = id;
    }
    info.jobId = mem::GetJobId(g_dataModel);
    info.clientVersion = off::RunningVersion();
}

}  // namespace

const char* StageName(Stage stage) {
    switch (stage) {
        case Stage::Image:         return "Image";
        case Stage::Offsets:       return "Offsets";
        case Stage::DataModel:     return "DataModel";
        case Stage::ScriptContext: return "ScriptContext";
        case Stage::LuaState:      return "LuaState";
        case Stage::Api:           return "Api";
        case Stage::Environment:   return "Environment";
        case Stage::Identity:      return "Identity";
        case Stage::Registry:      return "Registry";
        case Stage::Scheduler:     return "Scheduler";
        case Stage::Ipc:           return "Ipc";
        case Stage::Bootstrap:     return "Bootstrap";
        case Stage::Canary:        return "Canary";
        case Stage::Ready:         return "Ready";
        default:                   return "None";
    }
}

Stage CurrentStage() { return g_stage; }
void ReportStage(Stage stage) {
    g_stage = stage;
    log::Info("core: stage %s", StageName(stage));
}

bool Running() { return g_running.load(); }
bool UnloadRequested() { return g_unload.load() || ipc::UnloadRequested(); }

// ---------------------------------------------------------------- teleport queue
void QueueOnTeleport(const std::string& source) {
    AcquireSRWLockExclusive(&g_teleportLock);
    if (g_teleportQueue.size() < 64) g_teleportQueue.push_back(source);
    ReleaseSRWLockExclusive(&g_teleportLock);
}

std::vector<std::string> SnapshotTeleportQueue() {
    AcquireSRWLockShared(&g_teleportLock);
    const std::vector<std::string> copy = g_teleportQueue;
    ReleaseSRWLockShared(&g_teleportLock);
    return copy;
}

size_t TeleportQueueSize() {
    AcquireSRWLockShared(&g_teleportLock);
    const size_t size = g_teleportQueue.size();
    ReleaseSRWLockShared(&g_teleportLock);
    return size;
}

// ---------------------------------------------------------------- init
void Initialize() {
    ReportStage(Stage::Image);
    g_clientBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const pe::ImageInfo image = pe::Inspect(g_clientBase);
    if (!image.valid) {
        Fail(Stage::Image, "the client image headers did not parse");
        return;
    }
    seh::SetOwnImageRange(g_clientBase, image.sizeOfImage);

    ReportStage(Stage::Offsets);
    off::Load(g_clientBase);
    if (!off::VersionMatches()) {
        log::Warn("core: the offset table is for %ls but this client is %s — continuing on "
                  "probes only, features whose probes fail stay off",
                  off::ExpectedVersion(),
                  off::RunningVersion()[0] ? off::RunningVersion() : "(unknown)");
    }
    const wchar_t* workspace = inject::WorkspaceFromParams();
    g_workspace = workspace && *workspace ? workspace : DefaultWorkspace();
    api::SetWorkspaceRoot(g_workspace.c_str());
    log::Info("core: workspace = %ls", g_workspace.c_str());

    ReportStage(Stage::Ipc);
    if (!ipc::Start(GetCurrentProcessId())) {
        // Without IPC the user cannot see or control anything, but the module is
        // still valid; keep going and report through the log ring only.
        log::Error("core: IPC did not start — the UI will not be able to attach");
    } else {
        std::thread(AcceptThread).detach();
    }

    ReportStage(Stage::Api);
    if (lua::ResolveApi(g_clientBase) < 0) {
        Fail(Stage::Api, "required Luau entry points could not be resolved for this build "
                         "(docs/OFFSETS.md §4)");
        return;
    }
    lua::DumpResolution();

    ReportStage(Stage::DataModel);
    g_dataModel = mem::GetDataModel();
    if (!g_dataModel) {
        Fail(Stage::DataModel, "no DataModel in this process — WRONG_PROCESS; the UI will try "
                               "the next PID");
        return;
    }

    ReportStage(Stage::ScriptContext);
    g_scriptContext = mem::AcquireScriptContext(g_dataModel);
    if (!g_scriptContext) {
        Fail(Stage::ScriptContext, "ScriptContext did not validate against its class name");
        return;
    }

    // Everything that touches the VM must happen on the client's main thread, so
    // the rest of init is one bootstrap call there (see RequestBootstrap).
    ReportStage(Stage::Bootstrap);
    if (!inject::RequestMainThreadBootstrap(&BootstrapOnMainThread, nullptr)) {
        Fail(Stage::Bootstrap, "could not get onto the client main thread — no rendezvous, "
                               "so scripts could never run");
        return;
    }

    if (!g_bootstrapped.load()) {
        Fail(Stage::Bootstrap, "the main-thread bootstrap did not complete");
        return;
    }

    g_running.store(true);
    ReportStage(Stage::Ready);
    WatchdogThread();
}

// ---------------------------------------------------------------- bootstrap
bool BootstrapOnMainThread(void*) {
    // We are on the client's main thread. From here on, VM calls are legal.
    ReportStage(Stage::LuaState);
    g_state = lua::AcquireState(g_scriptContext);
    if (!g_state.L || !g_state.probed) {
        log::StageError("LuaState", "no probed lua_State (cache/signature/scan all failed)");
        return false;
    }
    sched::SetMainState(g_state.L);

    ReportStage(Stage::Environment);
    if (!lua::env::BuildEnvironment(g_state.L, g_env)) {
        log::StageError("Environment", "genv/renv/_G/shared could not be built");
        return false;
    }
    lua::env::SetSession(g_env);
    exec::SetEnvironment(g_env);

    ReportStage(Stage::Identity);
    const bool identityOk = lua::identity::ProbeRoundTrip(g_state.L);
    api::SetAvailable(api::Cap::Identity, identityOk);

    ReportStage(Stage::Registry);
    const bool registryOk = api::BuildAll(g_state.L, g_env, g_registry);
    api::SetAvailable(api::Cap::Env, registryOk && lua::GetApi().IsUsable());
    api::SetAvailable(api::Cap::Closures,
                      lua::GetApi().insert && lua::GetApi().iscfunction && lua::GetApi().pcall);
    api::SetAvailable(api::Cap::Hooks, lua::layout::kCanHookInPlace && lua::GetApi().topointer);
    api::SetAvailable(api::Cap::Metatable,
                      lua::GetApi().getmetatable && lua::GetApi().setmetatable);
    api::SetAvailable(api::Cap::Instances, lua::GetApi().pcall != nullptr);
    api::SetAvailable(api::Cap::Net, true);
    // fs needs both the API and a workspace root; without a root nothing can be
    // resolved safely, so the whole capability goes away rather than erroring per
    // call.
    api::SetAvailable(api::Cap::Fs, !g_workspace.empty());
    api::SetAvailable(api::Cap::Misc, true);
    uintptr_t gcListHead = 0;
    api::SetAvailable(api::Cap::Gc, off::CacheGet("gc.list_head", gcListHead));
    api::SetAvailable(api::Cap::Core, true);

    ReportStage(Stage::Scheduler);
    const sched::rendezvous::Strategy strategy =
        sched::rendezvous::InstallOnMainThread(g_state.L, g_env);
    if (strategy == sched::rendezvous::Strategy::None || !sched::Start()) {
        api::SetAvailable(api::Cap::Scheduler, false);
        log::StageError("Scheduler", "the Heartbeat drain could not be connected — EXECUTE will "
                                     "answer ERROR:Scheduler instead of running scripts");
    } else {
        api::SetAvailable(api::Cap::Scheduler, true);
    }

    // Drop PHETAMINE.internal now that the connection is held by the DLL: scripts
    // must not be able to drive the scheduler directly.
    lua::Api& luaApi = lua::GetApi();
    if (luaApi.getfield && luaApi.setfield && lua::env::PushGenv(g_state.L, g_env)) {
        if (luaApi.getfield(g_state.L, -1, "PHETAMINE")) {
            luaApi.pushnil(g_state.L);
            luaApi.setfield(g_state.L, -2, "internal");
            luaApi.pop(g_state.L, 1);
        }
        luaApi.pop(g_state.L, 1);
    }

    ReportStage(Stage::Canary);
    char canaryJson[1024];
    RunCanary(canaryJson, sizeof(canaryJson));
    ipc::PostCanary(canaryJson);

    ipc::Info info{};
    FillInfo(info);
    AcquireSRWLockExclusive(&g_infoLock);
    g_lastInfo = info;
    ReleaseSRWLockExclusive(&g_infoLock);
    ipc::PostReady(info);
    g_readyPosted.store(true);
    if (!ipc::Running()) log::Warn("core: IPC is down — Ready was written to the log ring only");

    g_bootstrapped.store(true);
    log::Info("core: bootstrap complete (version=%s, place=%llu, jobId=%s)",
              off::RunningVersion(), static_cast<unsigned long long>(info.placeId), info.jobId.c_str());
    return true;
}

// ---------------------------------------------------------------- canary
const char* RunCanary(char* buffer, size_t bufferSize) {
    lua::Api& api = lua::GetApi();
    bool envOk = false, closuresOk = false, identityOk = lua::identity::Resolved();
    bool fsOk = false, schedulerOk = sched::rendezvous::Armed();

    // env round-trip through the real registry: set genv.__pheta_canary = 42 and
    // read it back. This proves the table we hand to scripts is the table we can
    // write to.
    if (g_state.L && api.setfield && api.getfield) {
        __try {
            if (lua::env::PushGenv(g_state.L, g_env)) {
                api.pushnumber(g_state.L, 42.0);
                api.setfield(g_state.L, -2, "__pheta_canary");
                if (api.getfield(g_state.L, -1, "__pheta_canary")) {
                    const double value = api.tonumber ? api.tonumber(g_state.L, -1, nullptr) : 0.0;
                    envOk = (value == 42.0);
                    api.pop(g_state.L, 1);
                }
                api.pushnil(g_state.L);
                api.setfield(g_state.L, -2, "__pheta_canary");
                api.pop(g_state.L, 1);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            envOk = false;
        }
    }

    // closures: our registered C closures must be callable from Lua. We call
    // checkcaller() through the VM: it should answer true for a thread we made.
    if (g_state.L && api.getfield && api.pcall && api.pushcclosurek) {
        __try {
            if (lua::env::PushGenv(g_state.L, g_env) && api.getfield(g_state.L, -1, "checkcaller")) {
                api.remove(g_state.L, -2);
                if (api.pcall(g_state.L, 0, 1, 0) == lua::kOk) {
                    closuresOk = api.toboolean ? (api.toboolean(g_state.L, -1) != 0) : false;
                    api.pop(g_state.L, 1);
                }
            } else {
                api.pop(g_state.L, 1);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            closuresOk = false;
        }
    }

    if (!g_workspace.empty()) {
        const std::wstring probe = std::wstring(g_workspace) + L"\\.canary";
        HANDLE file = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_TEMPORARY, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(file, "ok", 2, &written, nullptr);
            CloseHandle(file);
            fsOk = DeleteFileW(probe.c_str()) != 0;
        }
    }

    api::SetAvailable(api::Cap::Env, envOk);
    api::SetAvailable(api::Cap::Closures, closuresOk);
    api::SetAvailable(api::Cap::Identity, identityOk);
    api::SetAvailable(api::Cap::Fs, fsOk);
    api::SetAvailable(api::Cap::Scheduler, schedulerOk);

    char caps[512];
    api::CapabilitiesJson(caps, sizeof(caps));
    std::snprintf(buffer, bufferSize,
                  "{\"env\":%s,\"closures\":%s,\"identity\":%s,\"fs\":%s,\"scheduler\":%s,"
                  "\"capabilities\":%s}",
                  envOk ? "true" : "false", closuresOk ? "true" : "false",
                  identityOk ? "true" : "false", fsOk ? "true" : "false",
                  schedulerOk ? "true" : "false", caps);
    log::Info("core: canary %s", buffer);
    return buffer;
}

// ---------------------------------------------------------------- rebind
void RebindOnMainThread() {
    ReportStage(Stage::DataModel);
    const uintptr_t dataModel = mem::GetDataModel();
    if (!dataModel) {
        log::Error("core: rebind found no DataModel — the client is mid-teleport; retrying next tick");
        ReportStage(Stage::Ready);
        return;
    }
    const uintptr_t scriptContext = mem::AcquireScriptContext(dataModel);
    if (!scriptContext) {
        log::Error("core: rebind could not re-acquire ScriptContext");
        ReportStage(Stage::Ready);
        return;
    }

    mem::ClearHiddenContainer();
    lua::env::ReleaseEnvironment(g_state.L, g_env);

    g_dataModel = dataModel;
    g_scriptContext = scriptContext;
    g_state = lua::AcquireState(scriptContext);
    if (!g_state.L) {
        log::Error("core: rebind could not re-acquire the VM state — the scheduler will stop");
        api::SetAvailable(api::Cap::Scheduler, false);
        return;
    }
    sched::SetMainState(g_state.L);

    if (!lua::env::BuildEnvironment(g_state.L, g_env)) {
        log::Error("core: rebind could not rebuild the environment");
        api::SetAvailable(api::Cap::Env, false);
        return;
    }
    lua::env::SetSession(g_env);
    exec::SetEnvironment(g_env);

    api::Registrar fresh{};
    if (!api::BuildAll(g_state.L, g_env, fresh)) {
        log::Error("core: rebind could not re-register the native API");
    }
    g_registry = fresh;

    // replay the teleport queue and autoexec
    std::string error;
    for (const std::string& source : SnapshotTeleportQueue()) {
        exec::Run(source, error);
    }
    log::Info("core: rebind complete — replayed %zu queued script(s)", TeleportQueueSize());

    char canaryJson[1024];
    RunCanary(canaryJson, sizeof(canaryJson));
    ipc::PostCanary(canaryJson);

    ipc::Info info{};
    FillInfo(info);
    ipc::PostReady(info);
    ReportStage(Stage::Ready);
}

// ---------------------------------------------------------------- watchdog
void WatchdogThread() {
    uint64_t lastJobIdHash = 0;
    int rebindAttempts = 0;

    while (!UnloadRequested()) {
        Sleep(500);

        // scheduler health: a stalled drain means every script silently stops
        // working, so it is a first-class failure rather than a warning.
        if (sched::rendezvous::Armed() && sched::DrainCount() > 0 && !sched::HeartbeatFresh()) {
            log::Error("core: the scheduler heartbeat stalls — marking the scheduler unhealthy");
            api::SetAvailable(api::Cap::Scheduler, false);
        }

        const uintptr_t dataModel = mem::GetDataModel();
        if (!dataModel) {
            if (++rebindAttempts > 3) {
                log::Fatal("core: DataModel has been gone for %d ticks — shutting down "
                           "(the client is probably closing)", rebindAttempts);
                g_unload.store(true);
                break;
            }
            continue;
        }
        rebindAttempts = 0;

        const std::string jobId = mem::GetJobId(dataModel);
        const uint64_t hash = std::hash<std::string>{}(jobId);
        if (hash != lastJobIdHash) {
            if (lastJobIdHash != 0) {
                log::Info("core: session changed (jobId now '%s') — rebinding", jobId.c_str());
                sched::Job job{};
                job.kind = sched::JobKind::Rebind;
                sched::Enqueue(job);
            }
            lastJobIdHash = hash;
        }
    }

    Shutdown();
}

// ---------------------------------------------------------------- shutdown
void Shutdown() {
    log::Info("core: shutdown starting (stage %s)", StageName(g_stage));
    g_running.store(false);

    ipc::Stop();
    sched::Stop();
    exec::StopAll();

    if (g_state.L) {
        lua::env::ReleaseEnvironment(g_state.L, g_env);
        lua::threads::Clear();
    }

    log::Info("core: shutdown complete — %llu job(s) processed, %u job(s) dropped, %zu log "
              "line(s) dropped",
              static_cast<unsigned long long>(sched::Processed()), sched::Dropped(),
              ipc::LogDropped());
    g_stage = Stage::None;

    // If the loader mapped this image by hand there is no module entry in the
    // PEB, so the process will never unload us: we unmap ourselves from a stub
    // page outside the image and terminate this thread inside ntdll.
    inject::SelfUnmapIfManual();
}



}  // namespace phetamine::core

// The scheduler's job handler is a thin forwarder: executor.cpp owns the job
// handlers, but the rebind pipeline lives here because it needs the core's state.
namespace phetamine::sched::jobs {
void RebindOnMainThread() { phetamine::core::RebindOnMainThread(); }
}  // namespace phetamine::sched::jobs
