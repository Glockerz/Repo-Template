// core/core.h — the init/shutdown pipeline.
//
// DllMain does exactly two things: DisableThreadLibraryCalls and start
// core::Initialize on a fresh thread (docs/PHETAMINE.md §3). Nothing else may
// run under the loader lock, so every other step lives here.
//
// Failure policy (docs/PHETAMINE.md §8): any stage that fails logs
// `ERROR:<stage>` over IPC, runs Shutdown(), and unloads. A half-initialized
// module in a live client process is the worst possible outcome, so "fail
// closed, always unload, never idle" is the rule.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace phetamine::lua::env { struct Environment; }

namespace phetamine::core {

enum class Stage : uint8_t {
    None = 0,
    Image,        // PE sanity
    Offsets,      // table load + version check
    DataModel,
    ScriptContext,
    LuaState,
    Api,          // luau_* resolution
    Environment,
    Identity,
    Registry,
    Scheduler,
    Ipc,
    Bootstrap,    // main-thread bootstrap (shellcode hijack + Heartbeat connect)
    Canary,
    Ready,
};

const char* StageName(Stage stage);
Stage CurrentStage();
void  ReportStage(Stage stage);

// Runs on the thread DllMain created. Returns when init finishes (successfully
// or not); on failure it shuts down and asks the loader to unmap.
void Initialize();
void Shutdown();

bool Running();
bool UnloadRequested();          // set by IPC OP_UNLOAD or a fatal watchdog path

// ---- teleport queue ----------------------------------------------------------
// Lives in the DLL (not in any CoreGui marker folder), replayed by the watchdog
// after a session rebind. `queue_on_teleport` appends here.
void QueueOnTeleport(const std::string& source);
std::vector<std::string> SnapshotTeleportQueue();
size_t TeleportQueueSize();

// ---- rebind (teleport) --------------------------------------------------------
// Runs on the client main thread, from a scheduler job: re-acquire DataModel,
// ScriptContext, state, rebuild genv, replay queue_on_teleport + autoexec, and
// re-run the canary.
void RebindOnMainThread();

// ---- bootstrap ---------------------------------------------------------------
// The one-shot main-thread entry: connects the Heartbeat drain through the VM.
// Called from the hijacked thread (see inject/hijack) with the client's main
// state. Returns true when the scheduler is armed.
bool BootstrapOnMainThread(void* luaStatePtr);

// ---- canary ------------------------------------------------------------------
// Runs the per-capability self-test and returns the bitmap as JSON. Called after
// init and after every rebind, from the main thread.
const char* RunCanary(char* buffer, size_t bufferSize);

// ---- watchdog ----------------------------------------------------------------
void WatchdogThread();

}  // namespace phetamine::core
