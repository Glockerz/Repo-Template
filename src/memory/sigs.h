// memory/sigs.h — AOB signatures for the VM layer.
//
// The public offset dump does NOT contain VM entries (docs/OFFSETS.md §2/§4):
// there is no luau_compile, no lua_State, no identity field in it. Those are
// found by signature scan, and signatures are per-build work.
//
// ---------------------------------------------------------------------------
// THE PATTERNS BELOW ARE INTENTIONALLY EMPTY.
//
// An empty pattern resolves to nothing, which disables the capability that
// depends on it — that is the fail-closed behaviour the design requires, and it
// is the only honest state for a sanitised tree that has never run against the
// build in question. Filling these with guessed bytes ("they look like a
// prologue") is exactly the mistake docs/OFFSETS.md §1 forbids: a wrong match
// is a call into the middle of an unrelated function.
//
// To derive a pattern for your build:
//   1. build with -DPHETAMINE_DUMP_ON_BOOT=ON, attach, read the `dump` lines to
//      see which keys are unresolved;
//   2. locate the target function in a disassembler (its callers are usually
//      identifiable: luau_compile is called with a string and returns a buffer;
//      lua_resume is called by the task scheduler; lua_newthread allocates a
//      lua_State-sized object);
//   3. take a distinctive 12–24 byte window that does not contain a call/jump
//      displacement or an absolute address, write it below in IDA form,
//      and re-run the boot canary (docs/STATUS.md §4 step 2).
//
// A pattern that matches more than once is rejected by the resolver (it logs
// `ambiguous`), because "which of the six matches is the VM function" is not a
// question a scanner should answer.
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>

namespace phetamine::scan::sigs {

struct Sig {
    const char* key;       // resolver key (logged, and used as the cache key)
    const char* gates;     // capability name that depends on this resolution
    const char* bytes;     // IDA-style pattern, e.g. "48 89 5C 24 ?? 57 48 83 EC"; "" = not derived
    int         ripDisp;   // 0 ⇒ the match address is the answer;
                           // >0 ⇒ a 32-bit RIP displacement starts at match+ripDisp
    int         ripLen;    // instruction end for the RIP-relative fixup (match + ripLen + disp)
    int         maxMatches;// reject a scan that produces more than this (0 = default 1)
};

inline constexpr Sig kSigs[] = {
    // ---- Luau compiler / loader -------------------------------------------
    { "luau.compile",          "executor.engine",  "", 0, 0, 1 },
    { "luau.load",             "executor.engine",  "", 0, 0, 1 },
    // ---- VM entry points ---------------------------------------------------
    { "lua.resume",            "scheduler",        "", 0, 0, 1 },
    { "lua.pcall",             "closures",         "", 0, 0, 1 },
    { "lua.newthread",         "executor.threads", "", 0, 0, 1 },
    { "lua.pushcclosurek",     "registry",         "", 0, 0, 1 },
    { "lua.ref",               "env",              "", 0, 0, 1 },
    { "lua.unref",             "env",              "", 0, 0, 1 },
    { "lua.setfield",          "registry",         "", 0, 0, 1 },
    { "lua.setreadonly",       "metatable",        "", 0, 0, 1 },
    { "lua.setsafeenv",        "env",              "", 0, 0, 1 },
    { "lua.resetthread",       "executor.stop",    "", 0, 0, 1 },
    { "crt.free",              "executor.engine",  "", 0, 0, 1 },
    // ---- state / engine layout --------------------------------------------
    // `lua.state.instance` is the accessor form; the primary route is still the
    // ScriptContext slot scan in lua/state.cpp, which needs no signature.
    { "lua.state.instance",    "lua.state",        "", 0, 0, 1 },
    { "sched.rendezvous.site", "scheduler",        "", 0, 0, 1 },
    { "gc.list_head",          "getgc",            "", 0, 0, 1 },
    { "namecall.tstring",      "getnamecallmethod","", 0, 0, 1 },
};

// Keys a fully-featured build is expected to resolve. Reported at boot so a
// maintainer sees exactly what is missing after a client patch.
inline constexpr const char* kExpectedKeys[] = {
    "luau.compile", "luau.load", "lua.resume", "lua.pcall", "lua.newthread",
    "lua.pushcclosurek", "lua.ref", "lua.unref", "lua.setfield",
    "crt.free", "lua.state.instance",
};

}  // namespace phetamine::scan::sigs
