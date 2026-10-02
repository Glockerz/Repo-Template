# PHETAMINEUI

The control surface: **find PID → inject → EXECUTE**. Nothing else.

## What it is not

* It does not build a payload. There is no Lua, no bytecode, no container, no
  prelude, no module hijack. The two binaries it ships (`PHETAMINE.dll`,
  `PHETAMINE.stub.bin`) are produced by the C++ build and by
  `tools/gen/shellcode_blob.mjs`; the UI only reads them and copies them into the
  target process.
* It does not know an offset, a signature, or a capability. All of that is
  resolved in-process by the DLL, which reports the result over IPC.
* It is not a script editor product. The text box exists so a developer can run
  one script; `StringBuilder`-sized files and multi-tab editing are out of scope.
* It does not talk to any network service. The only transport is the named pipe
  `\\.\pipe\PHETAMINE_<pid>` (ADR-3).

## Debug only

`PHETAMINEUI.csproj` **fails the build** for any configuration other than
`Debug`:

```xml
<Target Name="RefuseReleaseConfiguration" BeforeTargets="PrepareForBuild"
        Condition="'$(Configuration)' != 'Debug'">
  <Error Text="PHETAMINEUI is Debug-only by design …" />
</Target>
```

The reason is a layout rule, not a preference: a Release build would drop
`PHETAMINEUI.exe` into a `bin/Release` folder next to the module it injects, and a
folder like that is what gets zipped and handed around. This repository ships
tools that are built on purpose, per session, by the person using them
(`README.md` § Scope).

## Building

```powershell
# 1. the module (Windows, MSVC or clang-cl, x64)
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release

# 2. the loader blob (Node, any OS)
node tools/gen/shellcode_blob.mjs --out build/PHETAMINE.stub.bin

# 3. the UI (Debug, always)
cd ui/PHETAMINEUI
dotnet build -c Debug
```

`dotnet build -c Release` fails on purpose. Do not remove the target to "make it
build": that target is the reason this directory cannot be shipped by accident.

## Runtime contract

| Event | UI behaviour |
|---|---|
| `Ready` | show session info (`placeId`, `jobId`, `userId`, `clientVersion`, offset table version) |
| `Canary` | repopulate the capability list from the reported bitmap |
| `Log` | append, colour-coded by level; `ERROR:<stage>` lines are errors |
| `ExecResult` | match the `id` sent with `Execute`, show `ok`/`message` |
| pipe closes | say so and stop pretending; the module was unloaded or the client exited |

The UI never retries an injection silently. If the pipe does not come up within
5 seconds it says the module did not initialise and leaves the client alone — a
second injection into a half-initialised process is worse than a clear failure.
