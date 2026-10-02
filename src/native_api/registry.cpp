#include "native_api/api.h"
#include "lua/state.h"
#include "lua/env.h"
#include "scheduler/rendezvous.h"
#include "common/log.h"
#include "common/seh.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace phetamine::api {
namespace {

bool g_available[static_cast<size_t>(Cap::Count)] = {
    true,   // Core is always on; the executor plumbing itself is the point
    false, false, false, false, false, false, false, false, false, false, false,
};

// Every C function we hand to the VM. `isexecutorclosure` compares against this
// list, so the answer cannot be spoofed from Lua.
constexpr size_t kMaxOurFunctions = 256;
const void* g_ourFunctions[kMaxOurFunctions]{};
size_t g_ourFunctionCount = 0;

// Declarations for every registry row: `FN(sym, "name", tu, Cap::X)` must have
// an `l_<sym>` in the named TU.
#define FN(sym, name, tu, cap) int l_##sym(lua_State*);
#include "native_api/registry.def"
#undef FN

struct Entry {
    const char* luaName;
    int (*fn)(lua_State*);
    Cap cap;
};

const Entry kEntries[] = {
#define FN(sym, name, tu, cap) { name, &l_##sym, cap },
#include "native_api/registry.def"
#undef FN
};

// Creates (or fetches) the shadow subtable `genv[left]` used for dotted names,
// WITHOUT touching the engine's own table: the shadow's __index points at the
// engine table, so reads pass through and our writes stay local.
bool PrepareShadow(lua_State* L, int genvIndex, const char* left) {
    lua::Api& api = lua::GetApi();
    const int genvAbs = genvIndex > 0 ? genvIndex : api.gettop(L) + genvIndex + 1;

    api.createtable(L, 0, 2);                       // [genv][shadow]
    const int shadowAbs = api.gettop(L);

    // shadow.__index = renv[left]  (the engine's table, read-only for us)
    api.createtable(L, 0, 1);                       // [genv][shadow][mt]
    api.pushvalue(L, kGlobalsIndex);                // [..][mt][renv]
    if (api.getfield) api.getfield(L, -1, left);    // [..][mt][renv][engine]
    api.pop(L, 1);                                  // [..][mt][engine]
    api.setfield(L, -2, "__index");                 // [..][mt]
    api.pushvalue(L, shadowAbs);                    // [..][mt][shadow]
    if (api.setmetatable) api.setmetatable(L, -2);  // [..][mt]
    api.pop(L, 1);                                  // [..]

    api.pushvalue(L, shadowAbs);                    // [..][shadow]
    api.setfield(L, genvAbs, left);                 // genv[left] = shadow
    return true;
}

}  // namespace

void NoteOurFunction(const void* fn) {
    if (!fn || g_ourFunctionCount >= kMaxOurFunctions) return;
    for (size_t i = 0; i < g_ourFunctionCount; i++) {
        if (g_ourFunctions[i] == fn) return;
    }
    g_ourFunctions[g_ourFunctionCount++] = fn;
}

bool IsOurFunction(const void* fn) {
    if (!fn) return false;
    for (size_t i = 0; i < g_ourFunctionCount; i++) {
        if (g_ourFunctions[i] == fn) return true;
    }
    return false;
}

const char* Name(Cap cap) {
    for (const CapInfo& info : kCapabilities) {
        if (info.cap == cap) return info.name;
    }
    return "unknown";
}

Cap CapOf(const char* name) {
    if (!name) return Cap::Count;
    for (const CapInfo& info : kCapabilities) {
        if (std::strcmp(info.name, name) == 0) return info.cap;
    }
    return Cap::Count;
}

bool Has(Cap cap) {
    const size_t index = static_cast<size_t>(cap);
    if (index >= static_cast<size_t>(Cap::Count)) return false;
    return g_available[index];
}

void SetAvailable(Cap cap, bool available) {
    const size_t index = static_cast<size_t>(cap);
    if (index >= static_cast<size_t>(Cap::Count)) return;
    const bool was = g_available[index];
    g_available[index] = available;
    if (was != available) {
        log::Info("api: capability '%s' %s", Name(cap), available ? "enabled" : "DISABLED");
    }
}

void CapabilitiesJson(char* out, size_t cap) {
    if (!out || cap < 8) return;
    size_t used = 0;
    used += static_cast<size_t>(std::snprintf(out + used, cap - used, "{"));
    bool first = true;
    for (const CapInfo& info : kCapabilities) {
        used += static_cast<size_t>(std::snprintf(out + used, cap - used, "%s\"%s\":%s",
                                                  first ? "" : ",", info.name,
                                                  Has(info.cap) ? "true" : "false"));
        first = false;
        if (used >= cap - 32) break;
    }
    std::snprintf(out + used, cap - used, "}");
}

bool Add(lua_State* L, Registrar& r, const char* luaName, int (*fn)(lua_State*), Cap cap) {
    lua::Api& api = lua::GetApi();
    if (!L || !fn || !luaName) return false;
    if (!Has(cap)) {
        r.skipped++;
        log::Info("api: skipping %s (capability '%s' is off)", luaName, Name(cap));
        return false;
    }
    if (!api.pushcclosurek || !api.setfield || !api.rawgeti) {
        r.skipped++;
        return false;
    }

    if (!api.rawgeti(L, kRegistryIndex, r.tableRef)) return false;   // [tbl]

    const char* dot = std::strchr(luaName, '.');
    if (!dot) {
        api.pushcclosurek(L, fn, luaName, 0, nullptr);               // [tbl][fn]
        api.setfield(L, -2, luaName);                                // [tbl]
        api.pop(L, 1);
        r.registered++;
        return true;
    }

    // dotted: build the shadow subtable, then set the field inside it
    const std::string left(luaName, static_cast<size_t>(dot - luaName));
    const std::string right(dot + 1);
    PrepareShadow(L, -1, left.c_str());                              // [tbl][shadow]
    if (!api.getfield || api.getfield(L, -1, left.c_str()) == 0) {   // [tbl][shadow][shadow]
        api.pop(L, 2);
        return false;
    }
    api.pushcclosurek(L, fn, right.c_str(), 0, nullptr);             // [tbl][shadow][shadow][fn]
    api.setfield(L, -2, right.c_str());                              // [tbl][shadow][shadow]
    api.pop(L, 3);                                                   // []
    r.registered++;
    return true;
}

bool BuildAll(lua_State* L, const lua::env::Environment& env, Registrar& out) {
    lua::Api& api = lua::GetApi();
    if (!L || !env.valid() || !api.IsUsable()) {
        log::Error("api: cannot build the registry (state/env/api must all be ready)");
        return false;
    }
    api.createtable(L, 0, static_cast<int>(sizeof(kEntries) / sizeof(kEntries[0])));
    out.tableRef = api.ref(L, -1);
    api.pop(L, 1);
    if (out.tableRef == lua::env::kNoRef) {
        log::Error("api: pinning the closure table failed");
        return false;
    }

    for (const Entry& entry : kEntries) {
        NoteOurFunction(reinterpret_cast<const void*>(entry.fn));
        Add(L, out, entry.luaName, entry.fn, entry.cap);
    }

    if (!lua::env::MergeRegistryIntoGenv(L, env, out.tableRef)) {
        log::Error("api: could not merge the closure table into genv");
        return false;
    }
    if (!InstallPhetaminetable(L, env)) {
        log::Warn("api: PHETAMINE table installation failed — capability() will be absent");
    }
    log::Info("api: registered %d function(s), skipped %d (capability off)",
              out.registered, out.skipped);
    return true;
}

bool InstallPhetaminetable(lua_State* L, const lua::env::Environment& env) {
    lua::Api& api = lua::GetApi();
    if (!L || !env.valid()) return false;
    if (!lua::env::PushGenv(L, env)) return false;                   // [genv]

    // PHETAMINE table (the capability row creates/extends it as needed)
    if (api.getfield && api.getfield(L, -1, "PHETAMINE") == 0) {     // [genv][nil]
        api.pop(L, 1);
        api.createtable(L, 0, 4);                                    // [genv][pm]
    }                                                                // [genv][pm]
    const int pmAbs = api.gettop(L);

    // PHETAMINE.internal: the drain handle the bootstrap connects. It is
    // REMOVED after the bootstrap succeeds (see core::BootstrapOnMainThread) so
    // scripts cannot drive the scheduler directly.
    api.createtable(L, 0, 2);                                        // [genv][pm][internal]
    api.pushcclosurek(L, &sched::rendezvous::HeartbeatCallback, "heartbeat", 0, nullptr);
    api.setfield(L, -2, "heartbeat");                                // [genv][pm][internal]
    api.setfield(L, pmAbs, "internal");                              // [genv][pm]

    api.pop(L, 1);                                                   // [genv]
    api.pop(L, 1);                                                   // []
    return true;
}

// ---- argument helpers ---------------------------------------------------------
int ArgCheck(lua_State* L, int index, int expectedType, const char* fnName) {
    lua::Api& api = lua::GetApi();
    const int actual = api.type ? api.type(L, index) : kTypeNil;
    if (actual != expectedType) {
        char message[256];
        std::snprintf(message, sizeof(message), "%s: bad argument #%d (type %d expected, got %d)",
                      fnName ? fnName : "?", index, expectedType, actual);
        api.pushstring(L, message);
        if (api.error) api.error(L);
    }
    return actual;
}

void PushNil(lua_State* L) { lua::GetApi().pushnil(L); }
void PushBool(lua_State* L, bool value) { lua::GetApi().pushboolean(L, value ? 1 : 0); }
void PushNumber(lua_State* L, double value) { lua::GetApi().pushnumber(L, value); }
void PushString(lua_State* L, const char* s) { lua::GetApi().pushstring(L, s ? s : ""); }

const char* ToString(lua_State* L, int index) {
    lua::Api& api = lua::GetApi();
    return api.tostring ? api.tostring(L, index) : nullptr;
}

}  // namespace phetamine::api
