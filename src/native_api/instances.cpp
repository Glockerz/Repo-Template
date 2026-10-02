// native_api/instances.cpp — instance-side functions.
//
// `gethui` is the interesting one and it is fully real: the container is created
// through the engine's own Lua (Instance.new + Parent) inside the C closure,
// which is legal because this closure runs on the client main thread during the
// drain. Parenting goes to PlayerGui, NOT CoreGui — the identity-2 input
// restriction crash is engine behaviour around CoreGui and has nothing to do
// with being internal (see docs/PHETAMINE.md §2.7 of the draft).
//
// The rest are honest about what the VM + offset table can and cannot do:
//   compareinstances  → rawequal, with the documented equivalence argument
//   cloneref          → unavailable: making a second userdata for one instance
//                       needs the instance→userdata push, which is unresolved.
//                       A "returns the same value" stub would be a lie.
//   getinstances      → empty table + a loud log while the registry walk is
//                       unresolved (documented partial, never a fabricated list)
#include "native_api/api.h"
#include "lua/state.h"
#include "lua/env.h"
#include "memory/instance_walker.h"
#include "memory/offsets.h"
#include "common/log.h"
#include "common/seh.h"

#include <string>

namespace phetamine::api {
namespace {

int g_huiRef = lua::env::kNoRef;

void Raise(lua_State* L, const char* message) {
    lua::Api& api = lua::GetApi();
    api.pushstring(L, message);
    if (api.error) api.error(L);
}

// Pushes game.Players.LocalPlayer.PlayerGui, or nothing (returns false).
bool PushPlayerGui(lua_State* L) {
    lua::Api& api = lua::GetApi();
    api.pushvalue(L, lua::kGlobalsIndex);                 // [renv]
    if (!api.getfield(L, -1, "game")) { api.pop(L, 1); return false; }
    api.remove(L, -2);                                     // [game]
    if (api.type(L, -1) == lua::kTypeNil) { api.pop(L, 1); return false; }

    api.getfield(L, -1, "Players");                        // [game][players]
    if (api.type(L, -1) == lua::kTypeNil) { api.pop(L, 2); return false; }
    api.getfield(L, -1, "LocalPlayer");                    // [..][players][lp]
    if (api.type(L, -1) == lua::kTypeNil) { api.pop(L, 3); return false; }
    api.getfield(L, -1, "PlayerGui");                      // [..][lp][gui]
    if (api.type(L, -1) == lua::kTypeNil) { api.pop(L, 4); return false; }
    // [game][players][lp][gui] → keep only gui
    api.insert(L, 1);
    api.settop(L, 1);
    return true;
}

}  // namespace

// gethui() → the hidden container, created once per session.
int l_gethui(lua_State* L) {
    lua::Api& api = lua::GetApi();
    if (g_huiRef != lua::env::kNoRef && api.rawgeti && api.rawgeti(L, kRegistryIndex, g_huiRef)) {
        return 1;                                          // cached
    }

    if (!PushPlayerGui(L)) {                               // [gui]
        Raise(L, "gethui: game.Players.LocalPlayer.PlayerGui is not available yet");
        return 0;
    }

    // Already there from a previous session? (e.g. after a rebind)
    if (api.getfield(L, -1, "PHETAMINE") && api.type(L, -1) != lua::kTypeNil) {
        g_huiRef = api.ref(L, -1);
        return 1;                                          // [gui][folder]
    }
    api.pop(L, 1);                                         // [gui]

    // Create it through the engine: Instance.new("Folder")
    api.pushvalue(L, lua::kGlobalsIndex);
    if (!api.getfield(L, -1, "Instance")) { api.pop(L, 2); Raise(L, "gethui: Instance is not available"); return 0; }
    api.remove(L, -2);                                     // [gui][Instance]
    if (api.getfield(L, -1, "new") == 0) { api.pop(L, 2); Raise(L, "gethui: Instance.new is not available"); return 0; }
    api.remove(L, -2);                                     // [gui][new]
    api.pushstring(L, "Folder");                           // [gui][new]["Folder"]
    if (api.pcall(L, 1, 1, 0) != lua::kOk) {                // [gui][folder]
        const char* err = api.tostring ? api.tostring(L, -1) : nullptr;
        log::Error("gethui: Instance.new('Folder') failed: %s", err ? err : "(no message)");
        api.pop(L, 2);
        Raise(L, "gethui: could not create the container");
        return 0;
    }
    api.pushstring(L, "PHETAMINE");
    api.setfield(L, -2, "Name");

    // parent → PlayerGui
    api.pushvalue(L, 1);                                   // [gui][folder][gui]
    api.setfield(L, -2, "Parent");                         // [gui][folder]

    g_huiRef = api.ref(L, -1);
    log::Info("gethui: created the hidden container under PlayerGui (ref %d)", g_huiRef);
    return 1;                                              // [gui][folder] → returns folder
}

// cloneref(inst) — a second Lua reference to the same instance needs the
// instance→userdata push. Returning the SAME userdata would silently fail every
// use of the function's purpose (bypassing identity-based comparisons), so this
// refuses instead.
int l_cloneref(lua_State* L) {
    (void)L;
    Raise(L, "cloneref: unavailable on this build (making a second userdata for one instance "
             "needs the instance->userdata push, which is unresolved). Returning the same "
             "reference would be a lie.");
    return 0;
}

// compareinstances(a, b) — with cloneref unavailable, one instance can only ever
// have one userdata in Lua, so rawequal IS the comparison here. Documented in
// UNC_COVERAGE.md; when cloneref becomes available this needs the instance
// pointer extraction (lua/layout.h kUserdataInstance) instead.
int l_compareinstances(lua_State* L) {
    lua::Api& api = lua::GetApi();
    if (api.type(L, 1) != lua::kTypeUserdata || api.type(L, 2) != lua::kTypeUserdata) {
        PushBool(L, false);
        return 1;
    }
    PushBool(L, api.rawequal ? api.rawequal(L, 1, 2) != 0 : false);
    return 1;
}

int l_getinstances(lua_State* L) {
    lua::Api& api = lua::GetApi();
    static bool warned = false;
    if (!warned) {
        warned = true;
        log::Info("getinstances: the instance registry walk is unresolved on this build — "
                  "returning an empty table (partial, docs/UNC_COVERAGE.md)");
    }
    api.createtable(L, 0, 0);
    return 1;
}

int l_getnilinstances(lua_State* L) { return l_getinstances(L); }

int l_firetouchinterest(lua_State* L) {
    (void)L;
    Raise(L, "firetouchinterest: unavailable on this build (the touch primitive table is "
             "unresolved)");
    return 0;
}

int l_getcallbackvalue(lua_State* L) {
    (void)L;
    PushNil(L);
    return 1;
}

int l_getconnections(lua_State* L) {
    (void)L;
    lua::Api& api = lua::GetApi();
    static bool warned = false;
    if (!warned) {
        warned = true;
        log::Info("getconnections: the signal node walk is unresolved — returning an empty table "
                  "(partial). Scripts should branch on PHETAMINE.capability('instances').");
    }
    api.createtable(L, 0, 0);
    return 1;
}

int l_fireclickdetector(lua_State* L) {
    (void)L;
    Raise(L, "fireclickdetector: unavailable on this build (the click primitive is unresolved)");
    return 0;
}

int l_fireproximityprompt(lua_State* L) {
    (void)L;
    Raise(L, "fireproximityprompt: unavailable on this build (the prompt primitive is unresolved)");
    return 0;
}

}  // namespace phetamine::api
