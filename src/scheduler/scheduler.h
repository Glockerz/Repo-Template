// scheduler/scheduler.h — the ONLY place the VM is touched.
//
// Roblox Luau is single-threaded and owned by the client's main thread.
// Resuming a thread from our own worker while the engine is mid-frame is the
// classic source of "crashes after ten minutes", so:
//
//   * Enqueue() is the only thing workers may do — a POD push into an SPSC ring;
//   * Drain() runs on the client main thread and is the only thing that talks
//     to Lua or instances;
//   * Drain() is non-blocking, exception-free, allocation-free (results go out
//     through the fixed-size IPC ring — see ipc::LogLine).
//
// Yielding scripts: `wait`/`task.wait` returns LUA_YIELD from lua_resume. The
// thread is parked with a resume time and resumed on a later drain, under a
// per-frame time budget and a cap, so a script with 500 task.spawns cannot
// hitch a frame.
#pragma once

#include <cstdint>
#include <cstddef>

struct lua_State;

namespace phetamine::sched {

enum class JobKind : uint8_t {
    None = 0,
    RunScript,        // data = bytecode (client-allocated), size, ref = envRef, arg = identity
    ToBeResumed,      // reserved: resume a parked thread by pointer (arg = lua_State*)
    SetIdentity,      // arg = identity, ref = unused
    Rebind,           // data = nullptr: re-acquire everything (teleport)
    RestoreInstances, // data = list of {ref, parentRef}
    StopScripts,      // retire every thread in ourThreads
    Shutdown,         // unblock and stop
    CallInto,         // data = function pointer, arg = context (used by the hijack bootstrap)
};

struct Job {
    JobKind  kind = JobKind::None;
    uint64_t id = 0;
    void*    data = nullptr;     // ownership transfers to the drain for RunScript
    size_t   size = 0;
    int      ref = 0;
    uintptr_t arg = 0;
};

inline constexpr size_t kQueueCapacity = 256;   // power of two; POD ring
inline constexpr size_t kMaxParkedThreads = 256;

inline constexpr double kFrameBudgetMs = 1.5;   // drain + resume budget per frame
inline constexpr int    kMaxResumesPerFrame = 24;

// ---- job queue ---------------------------------------------------------------
// Returns the job id (monotonic, echoed back over IPC so the UI can correlate).
uint64_t Enqueue(const Job& job);

// Main thread only. Returns the number of jobs executed this call.
int Drain();

// Starts the drain driver (installs the rendezvous). Returns false when no
// strategy could be installed — in that case the `scheduler` capability is
// false and EXECUTE answers ERROR:Scheduler instead of running anything.
bool Start();
void Stop();

// ---- health ------------------------------------------------------------------
bool     HeartbeatFresh();       // true if the drain ran within the last ~2 s
uint32_t DrainCount();
uint64_t Processed();
uint32_t Dropped();              // jobs dropped because the ring was full

// ---- parked (yielding) threads ----------------------------------------------
void ParkYielded(lua_State* T, double resumeAtSeconds);
void RetireYielded(lua_State* T);
size_t ParkedCount();
void RetireAllParked();          // Stop/Shutdown path; does not call into Lua

// Resumes threads whose deadline passed, under the budget. Returns how many.
int ResumeDue();

// The client's main lua_State, recorded by Start()/the bootstrap. Used as the
// `from` argument when resuming parked threads.
void SetMainState(lua_State* L);
lua_State* MainState();

// ---- job handlers (implemented in executor/executor.cpp) ---------------------
// Declared here so the drain switch in scheduler.cpp has no block-scope
// declarations, and so executor.cpp is the only place that implements them.
namespace jobs {
void RunScript(const Job& job);
void SetIdentity(const Job& job);
void Rebind(const Job& job);
void StopScripts(const Job& job);
void ReportScriptError(lua_State* T, const char* message);

// Implemented in core/core.cpp (it needs the whole re-acquire pipeline).
void RebindOnMainThread();
}  // namespace jobs

}  // namespace phetamine::sched
