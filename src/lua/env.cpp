#include "lua/env.h"
#include "lua/state.h"
#include "common/log.h"
#include "common/seh.h"

namespace phetamine::lua::env {
namespace {

bool g_renvProtected = false;

// Pushes the value a ref points at. (lua_getref is a macro over
// lua_rawgeti(L, LUA_REGISTRYINDEX, ref) in the fork's lua.h.)
bool PushRef(lua_State* L, int ref) {
    Api& api = GetApi();
    if (!api.rawgeti || ref == kNoRef) return false;
    return api.rawgeti(L, kRegistryIndex, ref) != 0;
}

// Stack discipline for everything below: indices are converted to absolute
// values before any push, because negative indices shift as the stack grows —
// the classic source of "the metatable ended up on the wrong table".
int AbsIndex(lua_State* L, int idx) {
    Api& api = GetApi();
    if (idx > 0) return idx;
    return api.gettop(L) + idx + 1;
}

// env.__index = renv, via lua_setmetatable. Net stack effect: none.
bool LinkEnvToRenv(lua_State* L, int envIndex, int renvIndex) {
    Api& api = GetApi();
    const int envAbs = AbsIndex(L, envIndex);
    const int renvAbs = AbsIndex(L, renvIndex);

    if (!api.setmetatable) {
        log::Warn("env: lua_setmetatable is not bound — scripts will not see engine globals "
                  "through __index (reduced capability)");
        return false;
    }
    api.createtable(L, 0, 1);                  // [mt]
    api.pushvalue(L, renvAbs);                 // [mt][renv]
    api.setfield(L, -2, "__index");            // [mt]
    api.pushvalue(L, envAbs);                  // [mt][env]
    api.setmetatable(L, -2);                   // pop mt, install on env → []
    return true;
}

}  // namespace

bool BuildEnvironment(lua_State* L, Environment& out) {
    Api& api = GetApi();
    if (!L || !api.IsUsable()) {
        log::Error("env: cannot build an environment without a state and a bound API");
        return false;
    }
    out = Environment{};
    g_renvProtected = false;

    // ---- renv -------------------------------------------------------------
    api.pushvalue(L, kGlobalsIndex);           // [renv]
    if (api.type(L, -1) != kTypeTable) {
        log::Error("env: globals is not a table — refusing to build an environment");
        api.pop(L, 1);
        return false;
    }
    out.renvRef = api.ref(L, -1);
    api.pop(L, 1);                             // []
    if (out.renvRef == kNoRef) {
        log::Error("env: pinning renv failed");
        return false;
    }

    // ---- genv -------------------------------------------------------------
    api.createtable(L, 0, 128);                // [genv]
    out.genvRef = api.ref(L, -1);
    if (out.genvRef == kNoRef) {
        log::Error("env: pinning genv failed");
        api.pop(L, 1);
        return false;
    }
    if (!PushRef(L, out.renvRef)) {            // [genv][renv]
        log::Error("env: renv ref did not resolve");
        api.pop(L, 1);
        ReleaseEnvironment(L, out);
        return false;
    }
    LinkEnvToRenv(L, -2, -1);                  // [genv][renv]
    api.pop(L, 1);                             // [genv]

    // ---- _G ---------------------------------------------------------------
    api.createtable(L, 0, 8);                  // [genv][_G]
    out.gRef = api.ref(L, -1);
    if (out.gRef == kNoRef) {
        log::Error("env: pinning _G failed");
        api.pop(L, 2);
        ReleaseEnvironment(L, out);
        return false;
    }
    api.pushvalue(L, -1);                      // [genv][_G][_G]
    api.setfield(L, -3, "_G");                 // genv._G = _G → [genv][_G]
    api.pop(L, 1);                             // [genv]

    // ---- shared -----------------------------------------------------------
    api.createtable(L, 0, 8);                  // [genv][shared]
    out.sharedRef = api.ref(L, -1);
    if (out.sharedRef == kNoRef) {
        log::Error("env: pinning shared failed");
        api.pop(L, 2);
        ReleaseEnvironment(L, out);
        return false;
    }
    api.pushvalue(L, -1);                      // [genv][shared][shared]
    api.setfield(L, -3, "shared");             // genv.shared = shared → [genv][shared]
    api.pop(L, 1);                             // [genv]
    api.pop(L, 1);                             // []

    log::Info("env: built (renv=%d genv=%d _G=%d shared=%d)",
              out.renvRef, out.genvRef, out.gRef, out.sharedRef);
    ProtectRenv(L, out);
    return true;
}

bool PushGenv(lua_State* L, const Environment& env) {
    return PushRef(L, env.genvRef);
}

bool PushGenvRef(lua_State* L, int ref) {
    return PushRef(L, ref);
}

bool MergeRegistryIntoGenv(lua_State* L, const Environment& env, int registryRef) {
    Api& api = GetApi();
    if (!L || !env.valid() || registryRef == kNoRef) return false;
    if (!api.next) {
        log::Error("env: lua_next is not bound — cannot merge the registry table");
        return false;
    }
    if (!PushRef(L, env.genvRef)) return false;      // [genv]
    if (!PushRef(L, registryRef)) {                  // [genv][reg]
        log::Error("env: registry table ref did not resolve");
        api.pop(L, 1);
        return false;
    }

    int copied = 0;
    bool ok = true;
    __try {
        // [genv][reg][key] → next pushes value
        while (api.next(L, -2)) {
            // stack: [genv][reg][key][value]
            api.pushvalue(L, -4);                    // genv
            api.pushvalue(L, -3);                    // key
            api.pushvalue(L, -3);                    // value
            api.settable(L, -3);                     // genv[key] = value
            api.pop(L, 1);                           // drop value, keep key for the next iteration
            if (++copied >= 512) {
                log::Warn("env: registry merge stopped at the 512-entry cap (is the table wrong?)");
                break;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
        log::Error("env: registry merge faulted after %d entries", copied);
    }
    api.pop(L, 2);                                   // [genv][reg] → []
    if (ok) log::Info("env: merged %d function(s) into genv", copied);
    return ok;
}

void ProtectRenv(lua_State* L, const Environment& env) {
    Api& api = GetApi();
    if (g_renvProtected || !env.valid()) return;
    if (!api.setreadonly) {
        log::Warn("env: lua_setreadonly is not bound — renv stays writable "
                  "(matches upstream Luau; reduced capability)");
        return;
    }
    __try {
        api.pushvalue(L, kGlobalsIndex);         // [renv]
        api.setreadonly(L, -1, 1);
        api.pop(L, 1);
        g_renvProtected = true;
        log::Info("env: renv marked readonly");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        log::Error("env: lua_setreadonly faulted — leaving renv writable");
    }
}

void ReleaseEnvironment(lua_State* L, Environment& env) {
    Api& api = GetApi();
    if (!L) return;
    auto drop = [&](int& ref) {
        if (ref != kNoRef && api.unref) {
            __try { api.unref(L, ref); } __except (EXCEPTION_EXECUTE_HANDLER) { /* best effort */ }
        }
        ref = kNoRef;
    };
    drop(env.sharedRef);
    drop(env.gRef);
    drop(env.genvRef);
    drop(env.renvRef);
    g_renvProtected = false;
}

}  // namespace phetamine::lua::env
