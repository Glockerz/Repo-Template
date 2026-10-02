// lua/state.h — the VM binding.
//
// Signatures are the Luau fork's, verified against the fork's public headers
// (docs/PHETAMINE.md §5 records the differences from stock Lua 5.4 that matter):
//
//   luau_compile(const char* src, size_t n, lua_CompileOptions*, size_t* out) -> char*
//   luau_load(lua_State*, const char* chunk, const char* data, size_t n, int env) -> int
//   lua_resume(lua_State*, lua_State* from, int narg) -> int          // no nresults out
//   lua_pushcclosurek(lua_State*, lua_CFunction, const char* debugname, int nup, lua_Continuation)
//   lua_resetthread(lua_State*) -> void                               // NOT int
//   lua_ref/lua_unref, lua_setreadonly, lua_setsafeenv                // fork extensions
//
// Resolution is layered (memory/scanner.h): exports → address cache → signature
// → (optionally) the bundled Luau build → unset. Every pointer is NULL when
// unresolved, and every consumer checks before calling.
#pragma once

#include <cstdint>
#include <string>

struct lua_State;

namespace phetamine::lua {

// ---- entry point types -------------------------------------------------------
using compile_fn     = char*      (*)(const char* source, size_t size, void* options, size_t* outSize);
using load_fn        = int        (*)(lua_State* L, const char* chunkname, const char* data, size_t size, int env);
using resume_fn      = int        (*)(lua_State* L, lua_State* from, int narg);
using pcall_fn       = int        (*)(lua_State* L, int nargs, int nresults, int errfunc);
using newthread_fn   = lua_State* (*)(lua_State* L);
using pushcclosure_fn= void       (*)(lua_State* L, int (*fn)(lua_State*), const char* debugname, int nup, void* cont);
using ref_fn         = int        (*)(lua_State* L, int idx);
using unref_fn       = void       (*)(lua_State* L, int ref);
using getfield_fn    = int        (*)(lua_State* L, int idx, const char* k);
using setfield_fn    = void       (*)(lua_State* L, int idx, const char* k);
using rawget_fn      = void       (*)(lua_State* L, int idx);
using rawset_fn      = void       (*)(lua_State* L, int idx);
using setreadonly_fn = void       (*)(lua_State* L, int idx, int enabled);
using setsafeenv_fn  = void       (*)(lua_State* L, int idx, int enabled);
using resetthread_fn = void       (*)(lua_State* L);
using free_fn        = void       (*)(void* p);

using pushnil_fn     = void       (*)(lua_State* L);
using pushboolean_fn = void       (*)(lua_State* L, int b);
using pushnumber_fn  = void       (*)(lua_State* L, double n);
using pushstring_fn  = void       (*)(lua_State* L, const char* s);
using pushlstring_fn = void       (*)(lua_State* L, const char* s, size_t n);
using pushvalue_fn   = void       (*)(lua_State* L, int idx);
// lua_pushlightuserdata is a macro over lua_pushlightuserdatatagged(..., 0)
using pushlightud_fn = void       (*)(lua_State* L, void* p, int tag);
using pushcclosure_plain_fn = void (*)(lua_State* L, int (*fn)(lua_State*), int nup);

using type_fn        = int        (*)(lua_State* L, int idx);
using tostring_fn    = const char* (*)(lua_State* L, int idx);
using tolstring_fn   = const char* (*)(lua_State* L, int idx, size_t* len);
using tonumber_fn    = double     (*)(lua_State* L, int idx, int* isnum);
using toboolean_fn   = int        (*)(lua_State* L, int idx);
using tointeger_fn   = int        (*)(lua_State* L, int idx, int* isnum);   // lua_tointegerx
using tolightud_fn   = void*      (*)(lua_State* L, int idx);
using objlen_fn      = int        (*)(lua_State* L, int idx);

using gettop_fn      = int        (*)(lua_State* L);
using settop_fn      = void       (*)(lua_State* L, int idx);
using pop_fn         = void       (*)(lua_State* L, int n);
using insert_fn      = void       (*)(lua_State* L, int idx);
using remove_fn      = void       (*)(lua_State* L, int idx);
using replace_fn     = void       (*)(lua_State* L, int idx);
using rawgeti_fn     = int        (*)(lua_State* L, int idx, int n);
using rawseti_fn     = void       (*)(lua_State* L, int idx, int n);
using call_fn        = void       (*)(lua_State* L, int nargs, int nresults);
using iscfunction_fn = int        (*)(lua_State* L, int idx);
using tocfunction_fn = int (*)(lua_State* L, int idx);
using getfenv_fn     = void       (*)(lua_State* L, int idx);
using setfenv_fn     = int        (*)(lua_State* L, int idx);
using getupvalue_fn  = const char* (*)(lua_State* L, int funcindex, int n);
using setupvalue_fn  = const char* (*)(lua_State* L, int funcindex, int n);
using getthreaddata_fn = void*   (*)(lua_State* L);
using setthreaddata_fn = void    (*)(lua_State* L, void* data);
using stackdepth_fn  = int        (*)(lua_State* L);
using debugtrace_fn  = const char* (*)(lua_State* L);
using getinfo_fn     = int        (*)(lua_State* L, int level, const char* what, void* ar);
using gc_fn          = int        (*)(lua_State* L, int what, int data);
using tothread_fn    = lua_State* (*)(lua_State* L, int idx);
using topointer_fn   = const void* (*)(lua_State* L, int idx);
using status_fn      = int        (*)(lua_State* L);
using gettable_fn    = void       (*)(lua_State* L, int idx);
using settable_fn    = void       (*)(lua_State* L, int idx);
using createtable_fn = void       (*)(lua_State* L, int narr, int nrec);
using next_fn        = int        (*)(lua_State* L, int idx);
using getmetatable_fn= int        (*)(lua_State* L, int idx);
using setmetatable_fn= int        (*)(lua_State* L, int idx);
using rawequal_fn    = int        (*)(lua_State* L, int idx1, int idx2);
using equal_fn       = int        (*)(lua_State* L, int idx1, int idx2);
using error_fn       = void       (*)(lua_State* L);   // l_noret in the fork
using xmove_fn       = void       (*)(lua_State* from, lua_State* to, int n);

// Convenience indices. The fork computes these from LUAI_MAXCSTACK (8000), so
// LUA_REGISTRYINDEX = -(8000) - 2000 and LUA_GLOBALSINDEX = -(8000) - 2002.
inline constexpr int kGlobalsIndex = -10002;   // LUA_GLOBALSINDEX
inline constexpr int kRegistryIndex = -10000;  // LUA_REGISTRYINDEX
inline constexpr int kNoRef = -1;

// `lua_opcode`-free constants used by the API above.
inline constexpr int kUpvalueIndexBase = kGlobalsIndex;   // lua_upvalueindex(i) = kGlobalsIndex - i

// The fork's lua_Debug (transcribed from VM/include/lua.h; LUA_IDSIZE = 256).
struct Debug {
    const char* name = nullptr;
    const char* what = nullptr;
    const char* source = nullptr;
    const char* short_src = nullptr;
    int linedefined = 0;
    int currentline = 0;
    unsigned char nupvals = 0;
    unsigned char nparams = 0;
    char isvararg = 0;
    void* userdata = nullptr;
    char ssbuf[256]{};
};

// Type tags as they appear in TValue::tt (fork's lua_Type enum — NOT Lua 5.4's).
inline constexpr int kTypeNil       = 0;
inline constexpr int kTypeBoolean   = 1;
inline constexpr int kTypeLightUd   = 2;
inline constexpr int kTypeNumber    = 3;
inline constexpr int kTypeVector    = 4;
inline constexpr int kTypeString    = 5;
inline constexpr int kTypeTable     = 6;
inline constexpr int kTypeFunction  = 7;
inline constexpr int kTypeUserdata  = 8;
inline constexpr int kTypeThread    = 9;
inline constexpr int kTypeBuffer    = 10;
inline constexpr int kTypeNone      = -1;

inline constexpr int kMultiRet = -1; // LUA_MULTRET

inline constexpr int kOk = 0;        // LUA_OK
inline constexpr int kYield = 1;     // LUA_YIELD
inline constexpr int kErrRun = 2;    // LUA_ERRRUN
inline constexpr int kErrSyntax = 3; // LUA_ERRSYNTAX (compile errors surface via load)
inline constexpr int kErrMem = 4;
inline constexpr int kErrErr = 5;
inline constexpr int kBreak = 6;     // LUA_BREAK

enum class ApiSource : uint8_t { None, Exports, Cache, Signature, Bundled };

struct Api {
    // compiler + loader
    compile_fn      compile = nullptr;
    free_fn         free_buf = nullptr;      // the CLIENT's free, for compile() buffers
    load_fn         load = nullptr;
    resume_fn       resume = nullptr;
    pcall_fn        pcall = nullptr;
    newthread_fn    newthread = nullptr;
    pushcclosure_fn pushcclosurek = nullptr;
    ref_fn          ref = nullptr;
    unref_fn        unref = nullptr;
    getfield_fn     getfield = nullptr;
    setfield_fn     setfield = nullptr;
    rawget_fn       rawget = nullptr;
    rawset_fn       rawset = nullptr;
    setreadonly_fn  setreadonly = nullptr;   // fork-optional
    setsafeenv_fn   setsafeenv = nullptr;    // fork-optional
    resetthread_fn  resetthread = nullptr;   // fork-optional
    // stack
    pushnil_fn      pushnil = nullptr;
    pushboolean_fn  pushboolean = nullptr;
    pushnumber_fn   pushnumber = nullptr;
    pushstring_fn   pushstring = nullptr;
    pushlstring_fn  pushlstring = nullptr;
    pushvalue_fn    pushvalue = nullptr;
    pushlightud_fn  pushlightud = nullptr;
    pushcclosure_plain_fn pushcclosure = nullptr;
    type_fn         type = nullptr;
    tostring_fn     tostring = nullptr;
    tolstring_fn    tolstring = nullptr;
    tonumber_fn     tonumber = nullptr;
    toboolean_fn    toboolean = nullptr;
    tointeger_fn    tointeger = nullptr;
    tolightud_fn    tolightud = nullptr;
    objlen_fn       objlen = nullptr;
    gettop_fn       gettop = nullptr;
    settop_fn       settop = nullptr;
    pop_fn          pop = nullptr;
    insert_fn       insert = nullptr;
    remove_fn       remove = nullptr;
    replace_fn      replace = nullptr;
    rawgeti_fn      rawgeti = nullptr;
    rawseti_fn      rawseti = nullptr;
    call_fn         call = nullptr;
    iscfunction_fn  iscfunction = nullptr;
    tocfunction_fn  tocfunction = nullptr;
    getfenv_fn      getfenv = nullptr;
    setfenv_fn      setfenv = nullptr;
    getupvalue_fn   getupvalue = nullptr;
    setupvalue_fn   setupvalue = nullptr;
    getthreaddata_fn getthreaddata = nullptr;
    setthreaddata_fn setthreaddata = nullptr;
    stackdepth_fn   stackdepth = nullptr;
    debugtrace_fn   debugtrace = nullptr;
    getinfo_fn      getinfo = nullptr;
    gc_fn           gc = nullptr;
    tothread_fn     tothread = nullptr;
    topointer_fn    topointer = nullptr;
    status_fn       status = nullptr;
    gettable_fn     gettable = nullptr;
    settable_fn     settable = nullptr;
    createtable_fn  createtable = nullptr;
    next_fn         next = nullptr;
    getmetatable_fn getmetatable = nullptr;
    setmetatable_fn setmetatable = nullptr;
    rawequal_fn     rawequal = nullptr;
    equal_fn        equal = nullptr;
    error_fn        error = nullptr;
    xmove_fn        xmove = nullptr;

    ApiSource source = ApiSource::None;

    bool HasCore() const { return compile && load && resume && newthread && pushcclosurek; }
    bool HasStack() const {
        return gettop && settop && pushnil && tostring && type && pushstring &&
               insert && rawgeti && iscfunction && tocfunction;
    }
    bool IsUsable() const { return HasCore() && HasStack(); }
    const char* SourceName() const;
};

struct State {
    lua_State*  L = nullptr;             // the ScriptContext's global state
    uintptr_t   scriptContext = 0;
    uintptr_t   dataModel = 0;
    uintptr_t   globalState = 0;         // global_State* (from the layout scan, when configured)
    bool        probed = false;
};

// Resolves the API from the client image (layered). Returns how many entry
// points were bound, and logs every key it could not resolve.
int ResolveApi(uintptr_t clientBase);

// The bound API (never null after a successful ResolveApi of the core set).
Api& GetApi();

// Finds the ScriptContext's global lua_State*: cached address → signature →
// structural scan (only if lua/layout.h is configured) → fail.
State AcquireState(uintptr_t scriptContext);

// Behavioural probe: calls into the state and checks known globals. This is
// what turns "a pointer that looks like a lua_State" into "the VM".
bool ProbeBehaviour(const State& state);

// Logs the full resolution report (bound keys, sources, unresolved keys).
void DumpResolution();

}  // namespace phetamine::lua
