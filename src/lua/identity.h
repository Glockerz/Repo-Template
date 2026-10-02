// lua/identity.h — thread identity (what `getidentity`/`setidentity` mean).
//
// Identity is a *thread* property in the Roblox fork: it lives in the extra
// space hanging off the lua_State (the same space that carries the calling
// script). Two consequences the design leans on:
//
//   - every script runs on its own thread, so setting identity 8 for a script
//     cannot re-identify the engine's own threads;
//   - `setidentity` on a thread we do not own is refused, not attempted.
//
// The field locations are configuration (see lua/layout.h): 0 means "not
// configured", and identity-dependent features stay off rather than writing to
// a guessed offset. When configured, the write is validated by round-trip:
// set 8 → read back 8, on a thread we own, before the capability is enabled.
#pragma once

#include <cstdint>
#include <string>

struct lua_State;

namespace phetamine::lua::identity {

// Identity levels we care about: 2 = standard script, 8 = executor-elevated.
inline constexpr uintptr_t kStandard = 2;
inline constexpr uintptr_t kElevated = 8;

bool      Resolved();                       // layout configured AND round-trip passed
uintptr_t Get(lua_State* L);                // 0 when unresolved or unreadable
bool      Set(lua_State* L, uintptr_t identity);

// Runs the round-trip probe on a scratch thread created from `main`. Must be
// called on the main thread (it touches the VM). Returns false and disables the
// capability on any failure; never leaves the scratch thread registered.
bool ProbeRoundTrip(lua_State* main);

// Human-readable location, for the boot dump: "extra+0x0C8.identity".
std::string Describe();

// Calling script for a thread, when the extra space publishes it (used by
// getcallingscript). 0 when unresolved — callers return nil, not a fake.
uintptr_t GetCallingScript(lua_State* L);

}  // namespace phetamine::lua::identity
