// native_api/api.h — capabilities, registration, and the dispatcher.
//
// Every UNC function is a C closure registered from ONE table
// (native_api/registry.def). Adding a function is one row plus one `l_<symbol>`
// implementation in the matching translation unit:
//
//     FN(getgenv, "getgenv", env, Cap::Env)      → api::l_getgenv(lua_State*)
//
// The capability gate is the important part: a function whose resolution probe
// or canary entry failed is NOT registered, and `PHETAMINE.capability(name)`
// says so. A plausible-looking stub scores the same on UNC as an absent
// function and costs debugging time forever (docs/UNC_COVERAGE.md).
#pragma once

#include <cstdint>
#include <cstddef>

struct lua_State;

namespace phetamine::lua::env { struct Environment; }

namespace phetamine::api {

enum class Cap : uint32_t {
    Core,        // executor plumbing: loadstring, PHETAMINE table
    Env,
    Identity,
    Closures,
    Hooks,       // in-place closure replacement — needs closure-layout config
    Metatable,
    Instances,
    Net,
    Fs,
    Misc,
    Gc,
    Scheduler,
    Count,
};

struct CapInfo {
    const char* name;
    Cap         cap;
    const char* description;
};

inline constexpr CapInfo kCapabilities[] = {
    { "core",       Cap::Core,       "executor plumbing (loadstring, PHETAMINE.capability)" },
    { "env",        Cap::Env,        "getgenv/getrenv/getreg and the shared _G / shared tables" },
    { "identity",   Cap::Identity,   "thread identity read/write with a passed round-trip probe" },
    { "closures",   Cap::Closures,   "closure inspection, wrapping and cloning (no code patching)" },
    { "hooks",      Cap::Hooks,      "in-place closure hooking (requires closure layout configuration)" },
    { "metatable",  Cap::Metatable,  "raw metatables, metamethod hooks, readonly control" },
    { "instances",  Cap::Instances,  "instance walking, gethui, connections" },
    { "net",        Cap::Net,        "WinHTTP request() and its sync/async variants" },
    { "fs",         Cap::Fs,         "workspace-sandboxed file access" },
    { "misc",       Cap::Misc,       "clipboard, fps cap, teleport queue, hashing, crypt" },
    { "gc",         Cap::Gc,         "GC/registry enumeration (partial by design)" },
    { "scheduler",  Cap::Scheduler,  "main-thread rendezvous is armed and draining" },
};

Cap         CapOf(const char* name);
const char* Name(Cap cap);
bool        Has(Cap cap);              // probe + canary result, per session
void        SetAvailable(Cap cap, bool available);
void        CapabilitiesJson(char* out, size_t cap);

// ---- registration ------------------------------------------------------------
struct Registrar {
    int  tableRef = -1;        // pinned table the closures are collected in
    int  registered = 0;
    int  skipped = 0;
};

// Creates the closure table, runs every TU's registration, then merges it into
// genv. Main thread only (it touches the VM).
bool BuildAll(lua_State* L, const lua::env::Environment& env, Registrar& out);

// Pushes the PHETAMINE table (with .capability and the internal drain handles).
bool InstallPhetaminetable(lua_State* L, const lua::env::Environment& env);

// The set of C function pointers we registered — this is what
// `isexecutorclosure` consults, and it cannot be spoofed from Lua.
void NoteOurFunction(const void* fn);
bool IsOurFunction(const void* fn);

// ---- registration helpers (used by the TUs) ----------------------------------
// Adds one closure under `luaName`. A dotted name ("debug.info") creates a
// *shadow* subtable under genv whose __index points at the engine's table, so
// the engine's own `debug` is never mutated. Returns false when the capability
// is off, or when the VM call fails — both are counted, not thrown.
bool Add(lua_State* L, Registrar& r, const char* luaName, int (*fn)(lua_State*), Cap cap);

// Registration is driven entirely by registry.def: registry.cpp declares the
// `l_<symbol>` implementations and registers them in one pass, so a TU never
// needs its own registration entry point (and cannot drift out of sync).

// ---- workspace root (set by core from the loader's param block) --------------
void SetWorkspaceRoot(const wchar_t* root);
const wchar_t* WorkspaceRoot();

// ---- shared helpers for the TUs ---------------------------------------------
int  ArgCheck(lua_State* L, int index, int expectedType, const char* fnName);   // raises on mismatch
void PushNil(lua_State* L);
void PushBool(lua_State* L, bool value);
void PushNumber(lua_State* L, double value);
void PushString(lua_State* L, const char* s);
const char* ToString(lua_State* L, int index);

}  // namespace phetamine::api
