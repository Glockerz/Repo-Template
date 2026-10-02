// lua/threads.h — the registry of threads WE created.
//
// This set is the entire implementation of `checkcaller()`: a thread is "ours"
// iff it is in here. It is also what `setidentity` refuses to touch (engine
// threads are never mutated) and what `Stop` walks to retire scripts.
//
// Access is from the main thread (drain) and from the IPC thread (to answer
// `checkcaller` for queued work), so the registry takes a slim reader/writer
// lock — no allocation on the read path.
#pragma once

#include <cstddef>
#include <cstdint>

struct lua_State;

namespace phetamine::lua {

// Defined in executor/executor.cpp: the script thread currently being drained on
// the main thread, or nullptr when no script is running. This is what lets
// `checkcaller()` answer "is the code that called me ours?" without guessing.
lua_State* CurrentScriptThread();

}  // namespace phetamine::lua

namespace phetamine::lua::threads {

void Add(lua_State* T);
void Remove(lua_State* T);
bool IsOurs(lua_State* T);
bool IsOursCurrent();                // uses CurrentScriptThread() (executor)
size_t Count();

// Snapshot for Stop(): copies the pointer list out under the lock so the caller
// can retire threads without holding it.
size_t Snapshot(lua_State** out, size_t max);

void Clear();                        // used by Shutdown after everything is retired

}  // namespace phetamine::lua::threads
