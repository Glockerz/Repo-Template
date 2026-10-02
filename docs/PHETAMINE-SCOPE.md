# PHETAMINE — scope decision

**Status: DECLINED.** 2026-10-02, branch `arena/01a0fa53-repo-template`.
Recorded here so no future session silently inherits this as a task.

## What was requested

A native internal executor for Roblox: a manually-mapped DLL that owns the
Roblox Luau VM inside `RobloxPlayerBeta.exe`, a shellcode/thread-hijack
injection path, the UNC function surface as C closures, teleport-resilient
session rebinding, and an anti-detection posture aimed at the client's
integrity layer.

## Why it was declined

This is a cheat for a live online game. The players affected are the other
people in the same server, and there is no reading of the design in which the
deliverable — arbitrary unauthorized Lua execution inside a shipping Roblox
client — has a primary use other than cheating in multiplayer.

The scope note in the request ("build and test only against accounts/builds you
own") doesn't change it. Injecting code into the Roblox client to run scripts it
was not authorized to run is a circumvention of the platform's integrity and
terms, and the artifact is not account-scoped: the same DLL, mapper and
executor work against any server or player, including with an account you own.
So this is not a "how, within bounds" question — the component itself is the
problem, and I won't produce it in whole or in part.

## Not produced (in any form, including stubs, "just the loader", or "just the offsets")

- manual mapper / injector / shellcode-thread-hijack path
- VM state acquisition, thread identity spoofing, VM-lock-free execution
- the C-closure UNC surface (`hookfunction`, `hookmetamethod`, `newcclosure`, …)
- keyed offset/signature resolution against live client builds
- anti-detection or integrity-evasion work

No architecture document for PHETAMINE was written into this repo either — the
branch contains the template's protocol work plus this decision record, and
nothing that functions as a design blueprint for the above.

## What I can help with instead

These are real, adjacent engineering tasks, not consolation prizes:

1. **A standalone Luau host.** `luau-lang/luau` embedded in your own x64
   process: compile/load semantics, bytecode version pinning, sandboxed globals,
   coroutine/yield scheduling, resource limits. Useful for tools you own and
   fully document — no third-party client involved.
2. **Luau static analysis and tooling.** AST work on the open-source Luau
   parser, custom `luau-analyze` lint rules, formatter/minifier,
   `srcsyntax`-style source-transformation work. This is what `srcsyntax`'s
   syntax component is, minus the executor shell around it.
3. **Roblox Studio plugin development.** The sanctioned plugin API (widgets,
   automation, change-history tooling, asset/script analysis) — no injection,
   no client patching.
4. **Offsets research notebook.** How public build→offset/signature databases
   (e.g. the ImTheo offsets DB) are structured, PE version-resource keying,
   sigscan methodology, how to keep and diff a research record across builds —
   as documentation and analysis, not as a live resolution layer for a cheat.
5. **Windows internals as standalone teaching code.** PE format, manual
   mapping, SEH/VEH, named pipes, WinHTTP — implemented against **binaries you
   write**, where there is no victim process and no integrity layer to defeat.
6. **The UI and plumbing.** The WPF shell, named-pipe request/response framing,
   multi-tab editor, log/console pane, attach/enumerate UX — as a general
   desktop tooling exercise, decoupled from any game process.

Ask for any of the six by name and I'll build it properly.
