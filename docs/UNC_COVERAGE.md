# UNC coverage plan

Two different things, deliberately separated:

- **Status** — what exists in `src/` right now, and what gates it.
- **Score** — UNC is a behavioural test suite. The score is measured by running
  the suite against a live client; it is **not estimated here**. `docs/STATUS.md`
  carries the harness plan, this file carries the inventory.

Legend: `impl` written and probe-gated · `impl~` written but depends on a
fragile/optional resolution (degrades honestly) · `partial` returns an honest
subset and says so in its own log line · `raises` registered but always refuses
with a named error · `plan` designed, not written in this pass · `out`
deliberately not implemented.

Naming note: `print`, `warn` and `error` are the executor's *shadows* in `genv`
(the engine's own functions are reachable through `getrenv()`), and
`PHETAMINE.capability` lives on the `PHETAMINE` table. Dotted names
(`debug.info`, `crypt.encrypt`) are registered into a shadow subtable whose
`__index` is the engine's table, so nothing in the engine is mutated
(docs/DECISIONS.md ADR-9).

> Policy: **disable rather than fake.** A convincing stub scores the same as an
> absent function on UNC's behavioural checks, but costs debugging time forever.
> Anything unresolved is not registered, and `PHETAMINE.capability(name)` says so.

## Registry — every row in `src/native_api/registry.def`

`tools/verify.mjs` fails if a row here is missing a name below, if a name here is
registered twice, or if a row has no `l_<symbol>` in its translation unit.

| Function | TU | Status | Note |
|---|---|---|---|
| `getgenv` | env | impl | `genv` pinned in the registry at build time |
| `getrenv` | env | impl | the real globals table; readonly when the fork allows |
| `getreg` | env | partial | executor view (`globals`, `genv`, `mainthread`); the raw engine registry is not exposed by the fork's C API |
| `getsenv` | env | partial | real fenv for functions; nil for script instances (no instance→env map) |
| `gettenv` | env | impl | thread environment via `lua_getfenv` |
| `getfenv` | env | impl | pcall-aware stack level |
| `setfenv` | env | impl | refuses non-function targets |
| `getgc` | env | partial | GC list head unresolved ⇒ empty table + one log line |
| `print` | env | impl | shadow: forwards to the engine's `print` with an executor prefix |
| `warn` | env | impl | shadow: `warn` output goes to the IPC log at Warn level |
| `error` | env | impl | shadow: raises through the VM with the message unchanged |
| `getidentity` | identity | impl~ | extra-space field; capability stays off unless the round-trip probe passes |
| `setidentity` | identity | impl~ | refuses threads we do not own |
| `getthreadidentity` | identity | impl~ | explicit-thread variant |
| `setthreadidentity` | identity | impl~ | explicit-thread variant |
| `getthreadcontext` | identity | impl | `{ identity, caller, current }` for our threads |
| `iscclosure` | closures | impl | closure-struct tag |
| `islclosure` | closures | impl | closure-struct tag |
| `newcclosure` | closures | impl | Lua function as upvalue of a C closure |
| `clonefunction` | closures | impl | upvalues preserved |
| `hookfunction` | closures | **Cap::Hooks** | closure-pointer swap (no code patching); off unless the closure layout is configured |
| `unhookfunction` | closures | **Cap::Hooks** | restores from the hook bookkeeping table |
| `checkcaller` | closures | impl | current thread ∈ our thread set |
| `isexecutorclosure` | closures | impl | closure ∈ our registered pointer set |
| `getcallingscript` | closures | impl~ | nil unless the thread→script field is configured |
| `debug.info` | closures | impl | level/option-aware native implementation |
| `debug.traceback` | closures | impl | delegates to the fork's traceback when bound |
| `getscriptclosure` | closures | raises | script bookkeeping is unresolved; refuses by name |
| `getrawmetatable` | metatable | impl | `lua_getmetatable` (raw; ignores `__metatable`) |
| `setrawmetatable` | metatable | impl | `lua_setreadonly` bypass first |
| `hookmetamethod` | metatable | impl | closure swap in the metatable; original returned |
| `getnamecallmethod` | metatable | impl~ | namecall `TString*` from the thread; round-trip probed |
| `setnamecallmethod` | metatable | impl~ | writes the namecall string; probed |
| `setreadonly` | metatable | impl | fork helper |
| `isreadonly` | metatable | impl | fork helper |
| `gethui` | instances | impl | container created through the engine, parented to `PlayerGui` |
| `cloneref` | instances | raises | a second userdata for one instance needs the instance→userdata push; returning the same reference would be a lie |
| `compareinstances` | instances | impl | falls back to `rawequal` while `cloneref` is unavailable (documented in the source) |
| `getinstances` | instances | partial | registry walk unresolved ⇒ empty table + one log line |
| `getnilinstances` | instances | partial | same walk |
| `firetouchinterest` | instances | raises | touch primitive unresolved |
| `getcallbackvalue` | instances | partial | nil while the callback slots are unresolved |
| `getconnections` | instances | partial | empty table while the signal node walk is unresolved |
| `fireclickdetector` | instances | raises | click primitive unresolved |
| `fireproximityprompt` | instances | raises | prompt primitive unresolved |
| `request` | net | impl | WinHTTP, synchronous-form table in/out |
| `httpget` | net | impl | sync |
| `httppost` | net | impl | sync |
| `httpgetasync` | net | impl | worker + `CallInto` job; dropped jobs are counted, never re-entered on a worker |
| `getasync` | net | impl | alias of `httpgetasync` |
| `postasync` | net | impl | worker + `CallInto` job |
| `readfile` | fs | impl | workspace-sandboxed |
| `writefile` | fs | impl | creates parent folders |
| `appendfile` | fs | impl | workspace-sandboxed |
| `listfiles` | fs | impl | recursive, bounded at 4096 entries |
| `isfile` | fs | impl | |
| `isfolder` | fs | impl | |
| `makefolder` | fs | impl | |
| `delfile` | fs | impl | |
| `delfolder` | fs | impl | |
| `identifyexecutor` | misc | impl | `PHETAMINE` + version + build |
| `getexecutorname` | misc | impl | `PHETAMINE` |
| `setclipboard` | misc | impl | Win32 clipboard, called on a worker |
| `messagebox` | misc | impl | modal on a worker so the drain never blocks |
| `getfpscap` | misc | impl | `TaskScheduler::MaxFPS` read (0 when unresolved) |
| `setfpscap` | misc | impl~ | write + probe; a failed write marks the offset Failed |
| `queue_on_teleport` | misc | impl | DLL-resident queue, replayed by the rebind |
| `securecall` | misc | impl | `lua_pcall` wrapper; keeps `checkcaller()` answering true inside the call |
| `isrbxactive` | misc | impl | foreground-window check |
| `hash` | misc | impl | `sha256`, `sha1`, `md5` through CNG |
| `crypt.base64encode` | misc | impl | self-contained |
| `crypt.base64decode` | misc | impl | self-contained |
| `crypt.encrypt` | misc | impl | AES-CBC + PKCS#7 through CNG |
| `crypt.decrypt` | misc | impl | AES-CBC + PKCS#7 through CNG |
| `crypt.generatekey` | misc | impl | `BCryptGenRandom` (16/24/32 bytes) |
| `crypt.generateiv` | misc | impl | `BCryptGenRandom` (16 bytes) |
| `loadstring` | misc | impl | compiles with the client's `luau_compile`, frees with the client's `free` |
| `PHETAMINE.capability` | misc | impl | the bitmap scripts branch on |

## Tier 2 — should-have (not registered yet, by design)

| Item | Status | Note |
|---|---|---|
| `getloadedmodules`, `getrunningscripts`, `getscripts`, `getscripthash` | plan | needs the script bookkeeping map, which is also what `getscriptclosure`/`getcallingscript` want |
| `setsimulationradius`, `isnetworkowner` | plan | physics-side |
| `gethiddenproperty` / `sethiddenproperty` | plan | descriptor-lookup driven |
| `websocket.connect` | plan | `WinHttpWebSocket*` is in `net/client`; the missing piece is a scheduler-owned lifetime, not the API |
| `rconsole*` family | plan | engine console surface |
| `getconnections` (full enumeration) | plan | extends the partial walk above |
| `firetouchinterest` / `fireclickdetector` / `fireproximityprompt` | plan | needs the input-side primitives; the rows above refuse by name until then |

## Tier 3 — fragile or expensive

| Item | Status | Note |
|---|---|---|
| `getscriptbytecode` | plan | pinned to the client's Luau bytecode version; ships before any decompiler |
| `decompile` | out (this pass) | needs a real decompiler against the running bytecode version; the largest single work item |
| `saveinstance` | out (this pass) | instance serialiser + asset fetcher; large, separable |
| `Drawing` | out by policy | prefers a separate overlay window; `Present`/`SwapBuffers` hooks violate ADR-4 (no `.text` hooks) |

## Coverage accounting rules

1. A function counts as covered only when its **canary entry passes on the
   running build** and the UNC harness exercises it end-to-end.
2. `partial` implementations say so here **and** at runtime (log line + honest
   empty/nil result; never a fabricated table).
3. `raises` implementations are counted as **not covered** — they exist so a
   script gets a named error instead of `attempt to call a nil value`. When the
   underlying primitive resolves, the row becomes `impl` or it is deleted.
4. Any entry whose resolution probe fails is removed from the registry, so the
   harness records it as absent — by design, matching what a user would see.
