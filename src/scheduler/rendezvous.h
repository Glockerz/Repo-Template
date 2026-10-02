// scheduler/rendezvous.h — how the drain gets onto the client's main thread.
//
// Strategies, in the order the design prefers them:
//
//   Heartbeat  (default, implemented) — during the one-shot bootstrap the engine
//              calls a C closure we connect to RunService.Heartbeat. From then
//              on the ENGINE drives us, once per frame, at a point it already
//              considers legal. No signatures, no code patches, no offsets.
//
//   Apc        (implemented, off by default) — QueueUserAPC on a client thread.
//              An APC only runs during an alertable wait, which is exactly the
//              one property that makes it safe for VM work (it cannot land
//              inside Luau); but many threads never enter an alertable wait, so
//              viability is probed and reported rather than assumed.
//
//   TaskQueue / FrameSite (reserved) — the ScriptContext task-queue insert and
//              the once-per-frame call site. Both need a per-build address; when
//              one is put in the address cache under `sched.taskqueue.insert`
//              or `sched.rendezvous.site`, Install() will use it. They are
//              ordered after Heartbeat because they require writes/calls into
//              engine structures, which is a strictly larger footprint.
//
//   None       — honest failure: EXECUTE answers ERROR:Scheduler.
#pragma once

#include <cstdint>

struct lua_State;

namespace phetamine::lua::env { struct Environment; }

namespace phetamine::sched::rendezvous {

enum class Strategy : uint8_t { None = 0, Heartbeat, Apc, TaskQueue, FrameSite };

// Called on the client MAIN THREAD from the bootstrap: connects the drain to
// RunService.Heartbeat and returns the strategy that is now armed.
Strategy InstallOnMainThread(lua_State* L, const lua::env::Environment& env);

// Called from the init worker afterwards: returns the armed strategy, or tries
// the non-VM fallbacks (APC when explicitly enabled). Never touches Lua.
Strategy Install();

void Uninstall();
bool Armed();
const char* Name(Strategy);
Strategy Current();

// The C closure the engine calls once per frame. Deliberately tiny: drain, then
// return. It must never block, allocate, or throw.
int HeartbeatCallback(lua_State* L);

// APC viability probe: queues one throwaway APC that only bumps a counter, so we
// can report whether this client's threads ever reach an alertable wait. It does
// NOT run Lua — see the header note.
bool TryArmApcProbe();
uint32_t ApcProbeHits();

}  // namespace phetamine::sched::rendezvous
