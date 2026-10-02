#include "lua/state.h"
#include "memory/pe.h"
#include "lua/layout.h"
#include "memory/offsets.h"
#include "memory/scanner.h"
#include "memory/sigs.h"
#include "common/log.h"
#include "common/seh.h"

#include <windows.h>
#include <cstring>
#include <vector>

namespace phetamine::lua {
namespace {

Api g_api;
uintptr_t g_clientBase = 0;
pe::ImageInfo g_image{};

// key → where to store the resolved pointer. Order is the resolution order.
struct Entry {
    const char*  key;
    void**       slot;
    bool         required;   // required ⇒ the executor cannot run without it
};

template <typename T>
Entry bind(const char* key, T& fn, bool required) {
    return Entry{ key, reinterpret_cast<void**>(&fn), required };
}

std::vector<Entry> Entries() {
    std::vector<Entry> e;
    e.push_back(bind("luau.compile",      g_api.compile,       true));
    e.push_back(bind("crt.free",          g_api.free_buf,      true));
    e.push_back(bind("luau.load",         g_api.load,          true));
    e.push_back(bind("lua.resume",        g_api.resume,        true));
    e.push_back(bind("lua.pcall",         g_api.pcall,         false));
    e.push_back(bind("lua.newthread",     g_api.newthread,     true));
    e.push_back(bind("lua.pushcclosurek", g_api.pushcclosurek, true));
    e.push_back(bind("lua.ref",           g_api.ref,           true));
    e.push_back(bind("lua.unref",         g_api.unref,         false));
    e.push_back(bind("lua.setfield",      g_api.setfield,      true));
    e.push_back(bind("lua.getfield",      g_api.getfield,      false));
    e.push_back(bind("lua.rawget",        g_api.rawget,        false));
    e.push_back(bind("lua.rawset",        g_api.rawset,        false));
    e.push_back(bind("lua.setreadonly",   g_api.setreadonly,   false));
    e.push_back(bind("lua.setsafeenv",    g_api.setsafeenv,    false));
    e.push_back(bind("lua.resetthread",   g_api.resetthread,   false));
    e.push_back(bind("lua.pushnil",       g_api.pushnil,       true));
    e.push_back(bind("lua.pushboolean",   g_api.pushboolean,   true));
    e.push_back(bind("lua.pushnumber",    g_api.pushnumber,    true));
    e.push_back(bind("lua.pushstring",    g_api.pushstring,    true));
    e.push_back(bind("lua.pushlstring",   g_api.pushlstring,   false));
    e.push_back(bind("lua.pushvalue",     g_api.pushvalue,     true));
    e.push_back(bind("lua.pushlightuserdata", g_api.pushlightud, false));
    e.push_back(bind("lua.type",          g_api.type,          true));
    e.push_back(bind("lua.tostring",      g_api.tostring,      true));
    e.push_back(bind("lua.tolstring",     g_api.tolstring,     false));
    e.push_back(bind("lua.tonumber",      g_api.tonumber,      true));
    e.push_back(bind("lua.toboolean",     g_api.toboolean,     false));
    e.push_back(bind("lua.tointeger",     g_api.tointeger,     true));
    e.push_back(bind("lua.tolightuserdata", g_api.tolightud,   false));
    e.push_back(bind("lua.objlen",        g_api.objlen,        false));
    e.push_back(bind("lua.gettop",        g_api.gettop,        true));
    e.push_back(bind("lua.settop",        g_api.settop,        true));
    e.push_back(bind("lua.pop",           g_api.pop,           false));
    e.push_back(bind("lua.insert",        g_api.insert,        true));
    e.push_back(bind("lua.remove",        g_api.remove,        false));
    e.push_back(bind("lua.replace",       g_api.replace,       false));
    e.push_back(bind("lua.rawgeti",       g_api.rawgeti,       true));
    e.push_back(bind("lua.rawseti",       g_api.rawseti,       false));
    e.push_back(bind("lua.call",          g_api.call,          false));
    e.push_back(bind("lua.iscfunction",   g_api.iscfunction,   true));
    e.push_back(bind("lua.tocfunction",   g_api.tocfunction,   true));
    e.push_back(bind("lua.getfenv",       g_api.getfenv,       false));
    e.push_back(bind("lua.setfenv",       g_api.setfenv,       false));
    e.push_back(bind("lua.getupvalue",    g_api.getupvalue,    false));
    e.push_back(bind("lua.setupvalue",    g_api.setupvalue,    false));
    e.push_back(bind("lua.getthreaddata", g_api.getthreaddata, false));
    e.push_back(bind("lua.setthreaddata", g_api.setthreaddata, false));
    e.push_back(bind("lua.stackdepth",    g_api.stackdepth,    false));
    e.push_back(bind("lua.debugtrace",    g_api.debugtrace,    false));
    e.push_back(bind("lua.getinfo",       g_api.getinfo,       false));
    e.push_back(bind("lua.gc",            g_api.gc,            false));
    e.push_back(bind("lua.tothread",      g_api.tothread,      false));
    e.push_back(bind("lua.topointer",     g_api.topointer,     false));
    e.push_back(bind("lua.status",        g_api.status,        false));
    e.push_back(bind("lua.gettable",      g_api.gettable,      true));
    e.push_back(bind("lua.settable",      g_api.settable,      true));
    e.push_back(bind("lua.createtable",   g_api.createtable,   true));
    e.push_back(bind("lua.next",          g_api.next,          false));
    e.push_back(bind("lua.getmetatable",  g_api.getmetatable,  false));
    e.push_back(bind("lua.setmetatable",  g_api.setmetatable,  false));
    e.push_back(bind("lua.rawequal",      g_api.rawequal,      false));
    e.push_back(bind("lua.equal",         g_api.equal,         false));
    e.push_back(bind("lua.error",         g_api.error,         false));
    e.push_back(bind("lua.xmove",         g_api.xmove,         false));
    return e;
}

// The client does not export Luau symbols (it is statically linked), but the
// probe is free and it is the only *non-scan* source of truth, so it goes first.
void* FromExports(const char* key) {
    // strip our namespace: "luau.compile" → "luau_compile"
    char symbol[64]{};
    size_t n = 0;
    for (const char* p = key; *p && n < sizeof(symbol) - 2; p++) {
        symbol[n++] = (*p == '.') ? '_' : *p;
    }
    symbol[n] = '\0';
    HMODULE mod = reinterpret_cast<HMODULE>(g_clientBase);
    return reinterpret_cast<void*>(GetProcAddress(mod, symbol));
}

const char* SourceName(ApiSource s) {
    switch (s) {
        case ApiSource::Exports:   return "client exports";
        case ApiSource::Cache:     return "address cache";
        case ApiSource::Signature: return "signature";
        case ApiSource::Bundled:   return "bundled luau";
        default:                   return "unresolved";
    }
}

}  // namespace

const char* Api::SourceName() const {
    return phetamine::lua::SourceName(source);
}

int ResolveApi(uintptr_t clientBase) {
    g_clientBase = clientBase;
    g_image = pe::Inspect(clientBase);

    int bound = 0, requiredMissing = 0;
    ApiSource best = ApiSource::None;

    for (const Entry& e : Entries()) {
        if (*e.slot) { bound++; continue; }
        void* resolved = nullptr;
        ApiSource src = ApiSource::None;

        if (void* exp = FromExports(e.key)) { resolved = exp; src = ApiSource::Exports; }

        if (!resolved) {
            uintptr_t cached = 0;
            if (off::CacheGet(e.key, cached) && cached) { resolved = reinterpret_cast<void*>(cached); src = ApiSource::Cache; }
        }
        if (!resolved && g_image.valid) {
            const scan::Match m = scan::ResolveSig(g_image, e.key);
            if (m.address) { resolved = reinterpret_cast<void*>(m.address); src = ApiSource::Signature; }
        }
#if PHETAMINE_WITH_BUNDLED_LUAU
        if (!resolved) {
            if (void* bundled = bundled_vm::Lookup(e.key)) { resolved = bundled; src = ApiSource::Bundled; }
        }
#endif
        if (!resolved) {
            if (e.required) {
                requiredMissing++;
                log::Error("lua: REQUIRED entry point '%s' could not be resolved "
                           "(no export, no cache entry, no signature) — executor disabled", e.key);
            } else {
                log::Info("lua: optional entry point '%s' unresolved — dependent features off", e.key);
            }
            continue;
        }
        *e.slot = resolved;
        bound++;
        if (src == ApiSource::Signature) off::CachePut(e.key, reinterpret_cast<uintptr_t>(resolved));
        if (static_cast<int>(src) > static_cast<int>(best)) best = src;
        log::Info("lua: %-22s → %p  [%s]", e.key, resolved, SourceName(src));
    }

    g_api.source = best;
    log::Info("lua: api bound %d/%zu entry points (source: %s)%s",
              bound, Entries().size(), g_api.SourceName(),
              requiredMissing ? " — REQUIRED MISSING" : "");
    if (requiredMissing) return -requiredMissing;
    return bound;
}

Api& GetApi() { return g_api; }

// ---------------------------------------------------------------- state find

namespace {

bool LooksLikeState(uintptr_t candidate) {
    if (candidate < 0x10000 || (candidate & 0x7)) return false;
    if (!seh::IsReadable(candidate, 0x40)) return false;

    if (layout::kCanStructuralScan) {
        uint8_t tag = 0;
        if (!seh::TryRead<uint8_t>(candidate + layout::kStateTag, tag)) return false;
        if (tag != layout::kTagThread) return false;

        uintptr_t stack = 0;
        if (!seh::TryReadPtr(candidate + layout::kStateStack, stack) || stack < 0x10000) return false;

        int32_t stackSize = 0;
        if (!seh::TryRead<int32_t>(candidate + layout::kStateStackSize, stackSize)) return false;
        if (stackSize <= 20 || stackSize > layout::kMaxStackSize) return false;

        uintptr_t global = 0;
        if (!seh::TryReadPtr(candidate + layout::kStateGlobal, global) || global < 0x10000) return false;

        if (layout::kGlobalMainThread != layout::kUnset) {
            uintptr_t mainThread = 0;
            if (seh::TryReadPtr(global + layout::kGlobalMainThread, mainThread)) {
                if (mainThread != 0 && mainThread != candidate) return false;
            }
        }
        return true;
    }

    // Without configured layout we cannot verify structure, so we accept only
    // candidates that pass the *behavioural* probe later. Signal that here by
    // requiring read/write-ability of a plausible size.
    return seh::IsReadable(candidate, 0x100);
}

}  // namespace

State AcquireState(uintptr_t scriptContext) {
    State out{};
    out.scriptContext = scriptContext;
    if (!scriptContext) return out;

    // 1 · cached address for this build (still probed below)
    uintptr_t cached = 0;
    if (off::CacheGet("lua.state.instance", cached) && cached) {
        State candidate{};
        candidate.L = reinterpret_cast<lua_State*>(cached);
        candidate.scriptContext = scriptContext;
        if (ProbeBehaviour(candidate)) {
            out = candidate;
            out.probed = true;
            log::Info("lua: state from address cache: %p", static_cast<void*>(candidate.L));
            return out;
        }
        log::Warn("lua: cached state %p failed its probe — ignoring the cache for this session", (void*)cached);
    }

    // 2 · signature: an accessor `lua_State* f(ScriptContext*)`
    //     Contract for the `lua.state.instance` entry in memory/sigs.h: the
    //     matched address is called with the ScriptContext and must return the
    //     global state. Anything else (a field accessor, a thunk with a
    //     different convention) will fail the probe below and be discarded.
    if (g_image.valid) {
        const scan::Match m = scan::ResolveSig(g_image, "lua.state.instance");
        if (m.address) {
            using accessor_fn = lua_State* (*)(uintptr_t);
            lua_State* L = nullptr;
            __try {
                L = reinterpret_cast<accessor_fn>(m.address)(scriptContext);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                L = nullptr;
                log::Warn("lua: accessor candidate %p faulted — rejected", reinterpret_cast<void*>(m.address));
            }
            if (L && LooksLikeState(reinterpret_cast<uintptr_t>(L))) {
                State candidate{};
                candidate.L = L;
                candidate.scriptContext = scriptContext;
                if (ProbeBehaviour(candidate)) {
                    candidate.probed = true;
                    off::CachePut("lua.state.instance", reinterpret_cast<uintptr_t>(L));
                    log::Info("lua: state via accessor signature: %p", static_cast<void*>(L));
                    return candidate;
                }
            }
        }
    }

    // 3 · structural scan of the ScriptContext's own pointer slots
    if (layout::kCanStructuralScan) {
        std::vector<uintptr_t> candidates;
        for (uintptr_t off = 0; off < 0x800; off += 8) {
            uintptr_t value = 0;
            if (!seh::TryReadPtr(scriptContext + off, value)) continue;
            if (!LooksLikeState(value)) continue;
            candidates.push_back(value);
            if (candidates.size() > 8) break;
        }
        if (candidates.size() == 1) {
            State candidate{};
            candidate.L = reinterpret_cast<lua_State*>(candidates[0]);
            candidate.scriptContext = scriptContext;
            if (ProbeBehaviour(candidate)) {
                candidate.probed = true;
                off::CachePut("lua.state.instance", candidates[0]);
                log::Info("lua: state via structural scan: %p", static_cast<void*>(candidate.L));
                return candidate;
            }
        } else if (candidates.size() > 1) {
            log::Warn("lua: structural scan found %zu candidates — refusing to guess", candidates.size());
        }
    }

    log::StageError("LuaState",
                    "no lua_State found (cache=%s, layout configured=%s). "
                    "See lua/layout.h and docs/OFFSETS.md §4.",
                    cached ? "miss" : "empty",
                    layout::kCanStructuralScan ? "yes" : "no");
    return out;
}

bool ProbeBehaviour(const State& state) {
    if (!state.L) return false;
    const Api& api = g_api;
    if (!api.IsUsable()) {
        log::Error("lua: cannot probe a state without a bound API");
        return false;
    }

    lua_State* L = state.L;
    int top = 0;
    __try {
        top = api.gettop(L);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        log::Error("lua: probing %p faulted in lua_gettop — not a lua_State", (void*)L);
        return false;
    }
    if (top < 0 || top > 1'000'000) {
        log::Error("lua: probe read an implausible stack top (%d) from %p", top, (void*)L);
        return false;
    }

    bool ok = true;
    __try {
        // globals table, then the three names every Roblox state has
        api.pushvalue(L, kGlobalsIndex);
        if (api.type(L, -1) != kTypeTable) ok = false;

        if (ok) {
            api.getfield(L, -1, "game");
            ok = ok && api.type(L, -1) != kTypeNil;
            api.pop(L, 1);
        }
        if (ok) {
            api.getfield(L, -1, "print");
            ok = ok && api.type(L, -1) != kTypeNil;
            api.pop(L, 1);
        }
        if (ok) {
            api.getfield(L, -1, "task");
            ok = ok && api.type(L, -1) != kTypeNil;
            api.pop(L, 1);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }

    // always try to restore the stack: a probe must not leave the engine's main
    // thread with junk on top
    __try {
        api.settop(L, top);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }

    if (ok) log::Info("lua: state probe passed (top=%d, game/print/task present)", top);
    else    log::Error("lua: state probe FAILED — globals did not look like a Roblox state");
    return ok;
}

void DumpResolution() {
    log::Info("---- luau resolution ----");
    for (const Entry& e : Entries()) {
        log::Info("  %-24s %s", e.key, *e.slot ? "bound" : "UNRESOLVED");
    }
    log::Info("  source: %s", g_api.SourceName());
    log::Info("---- end ----");
}

}  // namespace phetamine::lua
