// native_api/env.cpp — environment functions.
//
// Honesty notes that shape this file:
//   * `getsenv` needs the script-instance → env mapping, which the VM API does
//     not expose; we answer it for FUNCTIONS (their real fenv) and return nil
//     for script instances rather than fabricating a table.
//   * `getgc`/`getreg` are partial by design (docs/UNC_COVERAGE.md Tier 2): what
//     we can answer truthfully (globals, main thread, our envs) is answered;
//     the GC walk is gated on a resolved list head and otherwise absent —
//     `Cap::Gc` stays off until that is configured.
#include "native_api/api.h"
#include "lua/state.h"
#include "lua/env.h"
#include "scheduler/scheduler.h"
#include "common/log.h"

#include <cstring>
#include <string>

namespace phetamine::api {
namespace {

bool g_warnedGc = false;

void PushJoinedArgs(lua_State* L, std::string& out) {
    lua::Api& api = lua::GetApi();
    const int n = api.gettop(L);
    for (int i = 1; i <= n; i++) {
        const char* s = nullptr;
        __try {
            s = api.tostring ? api.tostring(L, i) : nullptr;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            s = nullptr;
        }
        if (i > 1) out += "\t";
        out += s ? s : "?";
    }
}

}  // namespace

int l_getgenv(lua_State* L) {
    if (!lua::env::PushGenvRef(L, lua::env::Session().genvRef)) {
        PushNil(L);
    }
    return 1;
}

int l_getrenv(lua_State* L) {
    lua::GetApi().pushvalue(L, lua::kGlobalsIndex);
    return 1;
}

// Partial by design: a truthful registry view for our session. The fork's
// registry index is real (LUA_REGISTRYINDEX), so pushing it is honest; the
// extra fields are documented as the executor's view, not the raw engine table.
int l_getreg(lua_State* L) {
    lua::Api& api = lua::GetApi();
    api.createtable(L, 0, 4);                                   // [reg]

    api.pushvalue(L, lua::kGlobalsIndex);                       // [reg][globals]
    api.setfield(L, -2, "globals");

    if (lua::env::PushGenvRef(L, lua::env::Session().genvRef)) {
        api.setfield(L, -2, "genv");
    }
    if (api.pushlightud && sched::MainState()) {
        api.pushlightud(L, sched::MainState(), 0);
        api.setfield(L, -2, "mainthread");
    }
    if (!g_warnedGc) {
        g_warnedGc = true;
        log::Info("getreg: returning the executor's registry view (globals/genv/mainthread); "
                  "the raw engine registry table is not exposed by the fork's C API");
    }
    return 1;
}

// getsenv(script) — for functions this is their real fenv. For a script
// INSTANCE it needs the instance→env map, which we do not have; nil is the
// honest answer (docs/UNC_COVERAGE.md).
int l_getsenv(lua_State* L) {
    lua::Api& api = lua::GetApi();
    const int argType = api.type(L, 1);
    if (argType == lua::kTypeFunction && api.getfenv) {
        api.pushvalue(L, 1);
        api.getfenv(L, -1);
        api.remove(L, -2);
        return 1;
    }
    if (argType == lua::kTypeUserdata) {
        log::Info("getsenv: script-instance environments need the instance->env map "
                  "(unresolved on this build) — returning nil");
    }
    PushNil(L);
    return 1;
}

int l_gettenv(lua_State* L) {
    lua::Api& api = lua::GetApi();
    if (api.type(L, 1) != lua::kTypeThread || !api.getfenv) {
        PushNil(L);
        return 1;
    }
    api.pushvalue(L, 1);
    api.getfenv(L, -1);
    api.remove(L, -2);
    return 1;
}

int l_getfenv_native(lua_State* L) {
    lua::Api& api = lua::GetApi();
    const int index = api.type(L, 1) == lua::kTypeNumber ? static_cast<int>(api.tointeger(L, 1, nullptr)) : 1;
    if (!api.getfenv) {
        PushNil(L);
        return 1;
    }
    api.pushvalue(L, index);
    api.getfenv(L, -1);
    api.remove(L, -2);
    return 1;
}

int l_setfenv_native(lua_State* L) {
    lua::Api& api = lua::GetApi();
    if (!api.setfenv) {
        PushBool(L, false);
        return 1;
    }
    if (api.type(L, 1) != lua::kTypeFunction || api.type(L, 2) != lua::kTypeTable) {
        api.pushstring(L, "setfenv: function and table expected");
        if (api.error) api.error(L);
        return 0;
    }
    api.pushvalue(L, 1);        // [fn][env][fn]
    api.pushvalue(L, 2);        // [fn][env][fn][env]
    const int ok = api.setfenv(L, -2);   // pops env, sets it on fn
    api.pop(L, 1);              // [fn][env]
    api.pop(L, 1);              // [fn]
    PushBool(L, ok != 0);
    return 1;
}

// getgc(includeTables) — partial: the walk needs the GC list head, which is
// configuration (memory/sigs.h `gc.list_head`). Until it resolves, return an
// empty table and say so once; Cap::Gc keeps this out of reach by default.
int l_getgc(lua_State* L) {
    (void)L;
    lua::Api& api = lua::GetApi();
    static bool warned = false;
    if (!warned) {
        warned = true;
        log::Info("getgc: GC list head is not resolved on this build — returning an empty table "
                  "(partial, see docs/UNC_COVERAGE.md)");
    }
    api.createtable(L, 0, 0);
    return 1;
}

int l_shadow_print(lua_State* L) {
    std::string line;
    PushJoinedArgs(L, line);
    ipc::LogLine(ipc::Level::Info, line.c_str());
    return 0;
}

int l_shadow_warn(lua_State* L) {
    std::string line;
    PushJoinedArgs(L, line);
    ipc::LogLine(ipc::Level::Warn, line.c_str());
    return 0;
}

// error() must still raise, otherwise scripts that rely on it break silently.
int l_shadow_error(lua_State* L) {
    lua::Api& api = lua::GetApi();
    const char* message = api.tostring ? api.tostring(L, 1) : nullptr;
    ipc::LogLine(ipc::Level::Error, message ? message : "(error with no message)");
    if (api.error) {
        if (!message) api.pushstring(L, "error");
        else api.pushvalue(L, 1);
        api.error(L);            // does not return
    }
    PushNil(L);
    return 1;
}

}  // namespace phetamine::api
