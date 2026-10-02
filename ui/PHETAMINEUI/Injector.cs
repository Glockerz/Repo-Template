// Injector.cs — find the client, map the module, then get out of the way.
//
// Everything the DLL needs is handed over through a ParamBlock (see
// src/inject/param.h — the layout below must match it byte for byte) and a
// StubPayload (see src/inject/blob/README.md). The UI never builds Lua, never
// writes bytecode, and never touches the VM: its whole job is:
//
//   1. find RobloxPlayerBeta.exe and report the version the offsets were built for;
//   2. allocate the module image at its preferred base and copy it in;
//   3. allocate + write the stub blob, its payload descriptor, and the parameter
//      block;
//   4. start one thread at the blob's entry and read its exit code.
//
// The stub does the mapping (relocations, imports, protections) inside the
// target, because that is code we already trust to run there; the UI only
// manages memory and threads, which is the smallest possible surface.
using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

namespace PhetamineUI;

[StructLayout(LayoutKind.Sequential, Pack = 8, CharSet = CharSet.Unicode)]
internal struct ParamBlock
{
    public uint Magic;          // 0x504D5450 'PTMP'
    public uint Version;        // 1
    public uint StructSize;     // 816 — asserted on the C++ side too
    public uint Flags;          // 1 = manual map
    public ulong Reserved0;

    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)]
    public string Workspace;

    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)]
    public string PipeName;

    public uint Pid;
    public uint Reserved1;
    public ulong Reserved2;

    public const uint MagicValue = 0x504D5450;
    public const uint VersionValue = 1;
    public const uint FlagManualMap = 1;
    public const uint ExpectedSize = 816;

    public static ParamBlock Create(int pid, string workspace, string pipeName) => new()
    {
        Magic = MagicValue,
        Version = VersionValue,
        StructSize = ExpectedSize,
        Flags = FlagManualMap,
        Workspace = workspace.Length > 259 ? workspace[..259] : workspace,
        PipeName = pipeName.Length > 127 ? pipeName[..127] : pipeName,
        Pid = (uint)pid,
    };
}

internal sealed record InjectResult(bool Ok, string Message, IntPtr ImageBase, uint StubExitCode);

internal static class Injector
{
    private const uint MemCommitReserve = 0x3000;
    private const uint PageReadWrite = 0x04;
    private const uint PageExecuteRead = 0x20;
    private const uint ProcessAllAccess = 0x1F0FFF;
    private const uint WaitObject0 = 0x00000000;
    private const uint WaitTimeout = 0x00000102;

    public const string ProcessName = "RobloxPlayerBeta";

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr OpenProcess(uint access, bool inherit, int pid);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool CloseHandle(IntPtr handle);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr VirtualAllocEx(IntPtr process, IntPtr address, UIntPtr size, uint type, uint protect);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool VirtualProtectEx(IntPtr process, IntPtr address, UIntPtr size, uint newProtect, out uint oldProtect);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool WriteProcessMemory(IntPtr process, IntPtr address, byte[] buffer, UIntPtr size, out UIntPtr written);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr CreateRemoteThread(IntPtr process, IntPtr attributes, UIntPtr stackSize,
        IntPtr startAddress, IntPtr parameter, uint flags, out uint threadId);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern uint WaitForSingleObject(IntPtr handle, uint milliseconds);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool GetExitCodeThread(IntPtr handle, out uint exitCode);

    // ---- payload discovery ---------------------------------------------------
    public static string ModulePath => Path.Combine(AppContext.BaseDirectory, "PHETAMINE.dll");
    public static string StubPath => Path.Combine(AppContext.BaseDirectory, "PHETAMINE.stub.bin");

    public static string DefaultWorkspace
    {
        get
        {
            string root = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                "PHETAMINE", "workspace");
            Directory.CreateDirectory(Path.Combine(root, "autoexec"));
            Directory.CreateDirectory(Path.Combine(root, "scripts"));
            return root;
        }
    }

    public static List<Process> FindClients()
    {
        var list = new List<Process>();
        foreach (Process process in Process.GetProcessesByName(ProcessName)) list.Add(process);
        return list;
    }

    public static string ClientVersion(Process process)
    {
        try { return process.MainModule?.FileVersionInfo.FileVersion ?? "unknown"; }
        catch (Exception ex) { return $"unreadable ({ex.Message})"; }
    }

    // ---- the injection -------------------------------------------------------
    public static InjectResult Inject(Process process, string workspace)
    {
        if (process.HasExited) return new InjectResult(false, "the client has already exited", IntPtr.Zero, 0);

        byte[] moduleBytes;
        byte[] stubBytes;
        try
        {
            moduleBytes = File.ReadAllBytes(ModulePath);
            stubBytes = File.ReadAllBytes(StubPath);
        }
        catch (Exception ex)
        {
            return new InjectResult(false,
                $"cannot read the payload: {ex.Message}\n\n" +
                "Build them first:\n" +
                "  cmake --build build --config Release        (PHETAMINE.dll)\n" +
                "  node tools/gen/shellcode_blob.mjs --out build/PHETAMINE.stub.bin",
                IntPtr.Zero, 0);
        }

        if (!PeImage.TryParse(moduleBytes, out PeImage? module, out string peError) || module is null)
            return new InjectResult(false, $"PHETAMINE.dll is not a usable image: {peError}", IntPtr.Zero, 0);

        if (!module.IsDll || module.Machine != 0x8664)
            return new InjectResult(false,
                $"PHETAMINE.dll is not an x64 DLL (machine 0x{module.Machine:X4})", IntPtr.Zero, 0);

        if (module.HasRelocations)
            return new InjectResult(false,
                "PHETAMINE.dll carries base relocations; ADR-1 requires a reloc-free image so the " +
                "preferred-base map is deterministic. Rebuild it with the CMake preset in the repository " +
                "root (tools/verify.mjs does not check this — the build does).", IntPtr.Zero, 0);

        IntPtr target = OpenProcess(ProcessAllAccess, false, process.Id);
        if (target == IntPtr.Zero)
            return new InjectResult(false,
                $"OpenProcess failed ({new Win32Exception(Marshal.GetLastWin32Error()).Message}). " +
                "Run the UI elevated if the client is elevated.", IntPtr.Zero, 0);

        try
        {
            // 1. image at its preferred base.
            IntPtr imageBase = VirtualAllocEx(target, module.PreferredBase,
                (UIntPtr)module.SizeOfImage, MemCommitReserve, PageReadWrite);
            if (imageBase == IntPtr.Zero)
            {
                return new InjectResult(false,
                    $"VirtualAllocEx at the preferred base 0x{module.PreferredBase.ToInt64():X} failed " +
                    $"({new Win32Exception(Marshal.GetLastWin32Error()).Message}). " +
                    "Close and reopen the client, then try again.", IntPtr.Zero, 0);
            }

            byte[] image = module.BuildMappedImage(moduleBytes, imageBase);
            if (!Write(target, imageBase, image)) return Error("the module image could not be written", imageBase);

            // 2. stub + payload + parameters.
            IntPtr stubBase = VirtualAllocEx(target, IntPtr.Zero, (UIntPtr)(stubBytes.Length + 0x1000),
                MemCommitReserve, PageReadWrite);
            if (stubBase == IntPtr.Zero) return Error("the loader allocation failed", imageBase);

            IntPtr paramBase = VirtualAllocEx(target, IntPtr.Zero, (UIntPtr)1024, MemCommitReserve, PageReadWrite);
            if (paramBase == IntPtr.Zero) return Error("the parameter allocation failed", imageBase);

            IntPtr payloadBase = VirtualAllocEx(target, IntPtr.Zero, (UIntPtr)256, MemCommitReserve, PageReadWrite);
            if (payloadBase == IntPtr.Zero) return Error("the stub payload allocation failed", imageBase);

            if (!Write(target, stubBase, stubBytes)) return Error("the loader could not be written", imageBase);

            ParamBlock parameters = ParamBlock.Create(process.Id, workspace, $"PHETAMINE_{process.Id}");
            if (!Write(target, paramBase, StructureBytes(parameters)))
                return Error("the parameter block could not be written", imageBase);

            // StubPayload { void* image_base; u32 image_size; u32 entry_rva; void* param_block; }
            byte[] payload = new byte[32];
            BitConverter.GetBytes(imageBase.ToInt64()).CopyTo(payload, 0);
            BitConverter.GetBytes((uint)module.SizeOfImage).CopyTo(payload, 8);
            BitConverter.GetBytes(0u).CopyTo(payload, 12);   // 0 ⇒ the stub parses the export directory
            BitConverter.GetBytes(paramBase.ToInt64()).CopyTo(payload, 16);
            if (!Write(target, payloadBase, payload)) return Error("the stub payload could not be written", imageBase);

            // The loader is code now: RX, never RWX.
            if (!VirtualProtectEx(target, stubBase, (UIntPtr)(stubBytes.Length + 0x1000), PageExecuteRead, out _))
                return Error("the loader could not be made executable", imageBase);

            // 3. run it. The module stays RW until the stub fixes it up; the stub
            //    then applies the section protections from the image headers.
            IntPtr thread = CreateRemoteThread(target, IntPtr.Zero, UIntPtr.Zero, stubBase, payloadBase, 0, out uint threadId);
            if (thread == IntPtr.Zero) return Error("CreateRemoteThread failed", imageBase);

            uint wait = WaitForSingleObject(thread, 30_000);
            GetExitCodeThread(thread, out uint exitCode);
            CloseHandle(thread);

            if (wait == WaitTimeout)
                return new InjectResult(false, "the loader thread did not finish within 30 s", imageBase, exitCode);
            if (wait != WaitObject0)
                return new InjectResult(false, $"waiting for the loader failed (0x{wait:X8})", imageBase, exitCode);
            if (exitCode != 0)
                return new InjectResult(false,
                    $"the loader returned {exitCode}. Exit codes are documented in src/inject/stub/stub.c: " +
                    "2 = ntdll missing, 5 = kernel32 helpers missing, 8 = an import failed to resolve, " +
                    "9 = PhetamineEntry was not exported.", imageBase, exitCode);

            return new InjectResult(true,
                $"loader finished (thread {threadId}, image 0x{imageBase.ToInt64():X}) — the module is starting",
                imageBase, 0);

            InjectResult Error(string what, IntPtr baseAddress) => new(false,
                $"{what} ({new Win32Exception(Marshal.GetLastWin32Error()).Message})", baseAddress, 0);
        }
        finally
        {
            CloseHandle(target);
        }
    }

    private static bool Write(IntPtr process, IntPtr address, byte[] bytes)
    {
        return WriteProcessMemory(process, address, bytes, (UIntPtr)bytes.Length, out UIntPtr written) &&
               (long)written == bytes.Length;
    }

    private static byte[] StructureBytes<T>(T value) where T : struct
    {
        int size = Marshal.SizeOf<T>();
        byte[] buffer = new byte[size];
        IntPtr pointer = Marshal.AllocHGlobal(size);
        try
        {
            Marshal.StructureToPtr(value, pointer, false);
            Marshal.Copy(pointer, buffer, 0, size);
        }
        finally
        {
            Marshal.FreeHGlobal(pointer);
        }
        return buffer;
    }
}
