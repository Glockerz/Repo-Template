// lua/env.h — the environment model.
//
//   renv  : the client's real globals table (made readonly when the fork allows)
//   genv  : OUR table, one per session, with metatable.__index = renv
//   _G    : a separate shared table, registered into genv
//   shared: a separate shared table, registered into genv
//
// Every script thread is loaded with genv as its env, which is what makes
// `getgenv()` genuinely shared between scripts while `game`, `workspace`,
// `Instance.new` and `task` still resolve through __index to the real globals.
//
// Threading: BuildEnvironment and ReleaseEnvironment run on the CLIENT MAIN
// THREAD (they create and pin tables). Nothing here may be called from a worker.
#pragma once

#include <cstdint>

struct lua_State;

namespace phetamine::lua::env {

inline constexpr int kNoRef = -1;

struct Environment {
    int renvRef = kNoRef;
    int genvRef = kNoRef;
    int gRef = kNoRef;
    int sharedRef = kNoRef;

    bool valid() const { return genvRef != kNoRef; }
};

// Builds renv/genv/_G/shared, links genv.__index → renv and pins everything.
// Returns false (and releases what it built) rather than installing a
// half-built environment.
bool BuildEnvironment(lua_State* L, Environment& out);

// The live session environment. Owned by core (set once per bind, cleared on
// rebind/shutdown); the native_api translation units read it here instead of
// threading a context pointer through every C closure.
void SetSession(const Environment& env);
const Environment& Session();

// Pushes genv (from its ref). Caller owns the push.
bool PushGenv(lua_State* L, const Environment& env);

// Pushes the table a bare ref points at — used by scheduler jobs, which carry
// only the genv ref (never the whole Environment) across the queue.
bool PushGenvRef(lua_State* L, int ref);

// Copies key/value pairs from the pinned table `registryRef` into genv. This is
// how native_api/registry.cpp installs one row per UNC function.
bool MergeRegistryIntoGenv(lua_State* L, const Environment& env, int registryRef);

// Marks renv readonly when lua_setreadonly is bound; logs and continues
// otherwise (reduced capability, never a failure).
void ProtectRenv(lua_State* L, const Environment& env);

// Drops every ref. Called from core::Shutdown and on rebind, on the main thread.
void ReleaseEnvironment(lua_State* L, Environment& env);

}  // namespace phetamine::lua::env
