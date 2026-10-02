// native_api/closures.cpp — closure inspection, wrapping, cloning, hooking.
//
// What is real here (built on the fork's own C API — no struct offsets, no code
// patching):
//   iscclosure / islclosure   → lua_iscfunction
//   newcclosure               → a C closure with the Lua function as upvalue
//   clonefunction             → a distinct function object with the same
//                               behaviour (documented: not a proto duplicate,
//                               because Luau's C API does not expose one)
//   checkcaller               → thread ∈ ourThreads
//   isexecutorclosure         → C function pointer ∈ our registered set
//   debug.info / traceback    → lua_getinfo / lua_debugtrace
//   getcallingscript          → the extra-space field, nil when unresolved
//
// What is NOT here unless closure layout is configured:
//   hookfunction / unhookfunction (Cap::Hooks). An in-place swap needs the
//   closure's function/proto field; without it the only honest options are
//   "absent" or "a redirect table that existing references bypass" — and the
//   second one is a lie that breaks every script that calls the original
//   reference. So the capability stays off (docs/DECISIONS.md ADR-9).
#include "native_api/api.h"
#include "lua/state.h"
#include "lua/threads.h"
#include "lua/identity.h"
#include "executor/executor.h"
#include "common/log.h"

#include <cstdio>
#include <cstring>

namespace phetamine::api {
namespace {

void Raise(lua_State* L, const char* message) {
    lua::Api& api = lua::GetApi();
    api.pushstring(L, message);
    if (api.error) api.error(L);
}

// The upvalue index of the wrapped function inside our wrapper closures.
int WrappedIndex(lua_State* L) { return lua::kUpvalueIndexBase - 1; }

// Generic wrapper: calls the Lua function kept in upvalue 1 with the caller's
// arguments. Errors propagate through lua_error so the script keeps its own
// traceback.
int WrapperCall(lua_State* L) {
    lua::Api& api = lua::GetApi();
    const int nargs = api.gettop(L);
    api.pushvalue(L, WrappedIndex(L));            // [args][fn]
    api.insert(L, 1);                             // [fn][args]
    const int status = api.pcall(L, nargs, lua::kMultiRet, 0);
    if (status != lua::kOk) {
        const char* message = api.tostring ? api.tostring(L, -1) : nullptr;
        log::Error("newcclosure: wrapped function raised: %s", message ? message : "(no message)");
        if (api.error) api.error(L);
    }
    return lua::kMultiRet;
}

}  // namespace

int l_iscclosure(lua_State* L) {
    lua::Api& api = lua::GetApi();
    if (api.type(L, 1) != lua::kTypeFunction) {
        PushBool(L, false);
        return 1;
    }
    PushBool(L, api.iscfunction && api.iscfunction(L, 1) != 0);
    return 1;
}

int l_islclosure(lua_State* L) {
    lua::Api& api = lua::GetApi();
    if (api.type(L, 1) != lua::kTypeFunction) {
        PushBool(L, false);
        return 1;
    }
    PushBool(L, !(api.iscfunction && api.iscfunction(L, 1) != 0));
    return 1;
}

// newcclosure(fn) → a C closure (so scripts can hand it anywhere a C function is
// expected, e.g. as an event callback that must survive `checkcaller`).
int l_newcclosure(lua_State* L) {
    lua::Api& api = lua::GetApi();
    if (api.type(L, 1) != lua::kTypeFunction) {
        Raise(L, "newcclosure: function expected");
        return 0;
    }
    api.pushvalue(L, 1);                                   // [fn]
    api.pushcclosurek(L, &WrapperCall, "newcclosure", 1, nullptr);   // [wrapper]
    return 1;
}

// clonefunction(fn) → a distinct function object with identical behaviour.
// Documented difference from a proto clone: identity (rawequal) differs, and
// upvalues are shared rather than copied — the C API offers no way to copy a
// Proto, and pretending otherwise would be the "plausible stub" the design bans.
int l_clonefunction(lua_State* L) {
    lua::Api& api = lua::GetApi();
    if (api.type(L, 1) != lua::kTypeFunction) {
        Raise(L, "clonefunction: function expected");
        return 0;
    }
    api.pushvalue(L, 1);
    api.pushcclosurek(L, &WrapperCall, "clonefunction", 1, nullptr);
    return 1;
}

// ---- hooks (Cap::Hooks — off unless closure layout is configured) ------------
int l_hookfunction(lua_State* L) {
    (void)L;
    Raise(L, "hookfunction: this build has no configured closure layout, so an in-place hook "
             "is unavailable (docs/DECISIONS.md ADR-9). Refusing instead of installing a "
             "redirect that existing references would bypass.");
    return 0;
}

int l_unhookfunction(lua_State* L) {
    (void)L;
    Raise(L, "unhookfunction: no hooks are installed on this build");
    return 0;
}

int l_checkcaller(lua_State* L) {
    PushBool(L, lua::threads::IsOurs(L));
    return 1;
}

int l_isexecutorclosure(lua_State* L) {
    lua::Api& api = lua::GetApi();
    bool ours = false;
    if (api.type(L, 1) == lua::kTypeFunction && api.iscfunction && api.iscfunction(L, 1)) {
        const void* fn = api.tocfunction ? reinterpret_cast<const void*>(api.tocfunction(L, 1)) : nullptr;
        ours = IsOurFunction(fn);
    }
    PushBool(L, ours);
    return 1;
}

int l_getcallingscript(lua_State* L) {
    const uintptr_t script = lua::identity::GetCallingScript(L);
    if (!script) {
        // Documented: nil when the extra-space field is not resolved, rather than
        // a fabricated instance.
        PushNil(L);
        return 1;
    }
    // Without a resolved instance→userdata push, we can only hand back the raw
    // pointer as a light userdata; scripts that need the instance should use
    // `game` traversal instead. Honest and documented in UNC_COVERAGE.md.
    lua::Api& api = lua::GetApi();
    if (api.pushlightud) api.pushlightud(L, reinterpret_cast<void*>(script), 0);
    else PushNil(L);
    return 1;
}

// debug.info(levelOrFn, "slnau")
int l_debug_info(lua_State* L) {
    lua::Api& api = lua::GetApi();
    if (!api.getinfo) {
        Raise(L, "debug.info: lua_getinfo is not resolved on this build");
        return 0;
    }
    const char* what = api.tostring ? api.tostring(L, 2) : nullptr;
    if (!what) what = "sln";

    int level = 0;
    bool fromFunction = false;
    if (api.type(L, 1) == lua::kTypeNumber) {
        level = static_cast<int>(api.tointeger(L, 1, nullptr));
    } else if (api.type(L, 1) == lua::kTypeFunction) {
        api.pushvalue(L, 1);
        level = -1;                        // documented: level -1 means "this function"
        fromFunction = true;
    } else {
        Raise(L, "debug.info: number or function expected");
        return 0;
    }

    lua::Debug ar{};
    int ok = 0;
    __try {
        ok = api.getinfo(L, level, what, &ar);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = 0;
    }
    if (!ok) {
        Raise(L, "debug.info: lua_getinfo refused the request");
        return 0;
    }

    int results = 0;
    for (const char* c = what; *c; c++) {
        switch (*c) {
            case 's': PushString(L, ar.source ? ar.source : (ar.short_src ? ar.short_src : "?")); results++; break;
            case 'l': PushNumber(L, ar.currentline); results++; break;
            case 'n': PushString(L, ar.name ? ar.name : ""); results++; break;
            case 'a': PushNumber(L, ar.nparams); PushBool(L, ar.isvararg != 0); results += 2; break;
            case 'u': PushNumber(L, ar.nupvals); results++; break;
            case 'f':
                if (fromFunction) { api.pushvalue(L, 1); }
                else PushNil(L);
                results++;
                break;
            default: break;
        }
    }
    return results;
}

int l_debug_traceback(lua_State* L) {
    lua::Api& api = lua::GetApi();
    const char* message = api.type(L, 1) == lua::kTypeString && api.tostring ? api.tostring(L, 1) : nullptr;
    const char* trace = nullptr;
    if (api.debugtrace) {
        __try { trace = api.debugtrace(L); } __except (EXCEPTION_EXECUTE_HANDLER) { trace = nullptr; }
    }
    if (!trace) {
        // No traceback available: return the message unchanged rather than a
        // fabricated stack.
        PushString(L, message ? message : "");
        return 1;
    }
    if (message && *message) {
        char buffer[2048];
        std::snprintf(buffer, sizeof(buffer), "%s\n%s", message, trace);
        PushString(L, buffer);
    } else {
        PushString(L, trace);
    }
    return 1;
}

// getscriptclosure(script) — see UNC_COVERAGE.md Tier 2: needs the ScriptContext
// script bookkeeping, so it is absent until that resolves. The row exists so the
// UNC harness and the coverage table agree that the feature is *known and off*.
int l_getscriptclosure(lua_State* L) {
    (void)L;
    Raise(L, "getscriptclosure: not available on this build (script bookkeeping is unresolved)");
    return 0;
}

}  // namespace phetamine::api
