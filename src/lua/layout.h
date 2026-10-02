// lua/layout.h — lua_State / global_State field offsets.
//
// These are the ONLY struct offsets in the project that cannot come from a
// public dump, because the dump does not publish VM internals. They are
// therefore *configuration*, not constants: every value defaults to 0, which
// means "not configured for this Luau fork revision", and the state resolver
// refuses to scan when they are unset.
//
// How to fill them (bring-up, once per fork revision):
//   1. take the Luau revision the client is built from (see
//      third_party/Luau/README.md for how the fork is pinned);
//   2. read lstate.h in that revision and transcribe the offsets of the fields
//      below — they are structural, not patch-specific, and only change when
//      the fork's struct layout changes;
//   3. set the constants, rebuild, and confirm the boot canary reaches
//      `ERROR:`-free `READY` and that `getgenv()` round-trips.
//
// The resolver still validates every candidate *behaviourally* after the
// structural scan (see lua/state.cpp ProbeBehaviour), so a wrong value here
// fails closed instead of handing the executor a bogus state.
#pragma once

#include <cstdint>

namespace phetamine::lua::layout {

inline constexpr uintptr_t kUnset = 0;

// offset of `global_State* global` inside lua_State
inline constexpr uintptr_t kStateGlobal = kUnset;
// offset of the thread's type tag (must read back as kTagThread)
inline constexpr uintptr_t kStateTag = kUnset;
// offset of `TValue* stack`
inline constexpr uintptr_t kStateStack = kUnset;
// offset of `int stacksize`
inline constexpr uintptr_t kStateStackSize = kUnset;
// offset of `global_State::mainthread`
inline constexpr uintptr_t kGlobalMainThread = kUnset;

// ---- extra space (identity + calling script) --------------------------------
// The Roblox fork hangs an "extra space" struct off the thread. Transcribe from
// the same revision as the fields above.
//
//   offset of `ExtraSpace* extra` inside lua_State
inline constexpr uintptr_t kStateExtra = kUnset;
//   offset of the identity field inside ExtraSpace
inline constexpr uintptr_t kExtraIdentity = kUnset;
//   true  ⇒ identity is a byte (uint8_t) field
//   false ⇒ identity is a word (uintptr_t) field
inline constexpr bool kExtraIdentityIsByte = true;
//   offset of the calling-script pointer inside ExtraSpace (optional)
inline constexpr uintptr_t kExtraScript = kUnset;

// ---- namecall -----------------------------------------------------------------
//   offset of the namecall method TString* on lua_State (getnamecallmethod)
inline constexpr uintptr_t kNamecallTString = kUnset;

// ---- closures (only needed for in-place hooking — Cap::Hooks) -----------------
//   offset of the function pointer inside a C closure
inline constexpr uintptr_t kClosureFunction = kUnset;
//   offset of the Proto* inside a Lua closure
inline constexpr uintptr_t kClosureProto = kUnset;

inline constexpr bool kCanHookInPlace =
    kClosureFunction != kUnset || kClosureProto != kUnset;

// LUA_TTHREAD in the Luau fork. Verified against the fork's VM/include/lua.h
// (2026-10-02): LUA_TNIL=0 … LUA_TUSERDATA=8, LUA_TTHREAD=9, LUA_TBUFFER=10.
// NOTE: this is a *type tag* and has nothing to do with the executor identity
// level 8 — the draft spec conflated the two, and code that checked `== 8`
// would have been testing for LUA_TUSERDATA.
inline constexpr uint8_t kTagThread = 9;

// A structural scan is only possible when the fields it needs are configured.
inline constexpr bool kCanStructuralScan =
    kStateGlobal != kUnset && kStateTag != kUnset && kStateStack != kUnset && kStateStackSize != kUnset;

inline constexpr int kMaxStackSize = 1 << 20;   // sanity bound for stacksize

}  // namespace phetamine::lua::layout
