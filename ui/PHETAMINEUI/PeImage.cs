// PeImage.cs — the smallest PE reader that can lay an image out in memory.
//
// It exists for three questions only:
//   * is this a 64-bit DLL at all?
//   * does it carry base relocations? (ADR-1: the payload must not, because the
//     loader maps at the preferred base and reloc-free is what makes that
//     deterministic)
//   * what bytes belong at each offset of the mapped image?
//
// It deliberately does NOT resolve imports, apply relocations or touch the entry
// point: that is the stub's job, inside the target, and duplicating it here would
// create two implementations of the loader that could disagree.
using System.Buffers.Binary;

namespace PhetamineUI;

internal sealed class PeImage
{
    public ushort Machine { get; private init; }
    public bool IsDll { get; private init; }
    public bool HasRelocations { get; private init; }
    public IntPtr PreferredBase { get; private init; }
    public int SizeOfImage { get; private init; }
    public int SizeOfHeaders { get; private init; }
    private List<(int VirtualAddress, int VirtualSize, int RawPointer, int RawSize)> Sections { get; init; } = [];

    public static bool TryParse(byte[] file, out PeImage? image, out string error)
    {
        image = null;
        error = string.Empty;

        if (file.Length < 0x100) { error = "the file is too small to be an image"; return false; }
        if (BinaryPrimitives.ReadUInt16LittleEndian(file.AsSpan(0)) != 0x5A4D) { error = "no MZ signature"; return false; }

        int ntOffset = BinaryPrimitives.ReadInt32LittleEndian(file.AsSpan(0x3C));
        if (ntOffset <= 0 || ntOffset + 0x108 > file.Length) { error = "the NT header offset is out of range"; return false; }
        if (BinaryPrimitives.ReadUInt32LittleEndian(file.AsSpan(ntOffset)) != 0x00004550) { error = "no PE signature"; return false; }

        var span = file.AsSpan(ntOffset);
        ushort machine = BinaryPrimitives.ReadUInt16LittleEndian(span[4..]);
        int sectionCount = BinaryPrimitives.ReadUInt16LittleEndian(span[6..]);
        int optionalSize = BinaryPrimitives.ReadUInt16LittleEndian(span[20..]);
        ushort characteristics = BinaryPrimitives.ReadUInt16LittleEndian(span[22..]);

        int optional = ntOffset + 24;
        if (optional + optionalSize > file.Length) { error = "the optional header runs past the file"; return false; }
        if (BinaryPrimitives.ReadUInt16LittleEndian(file.AsSpan(optional)) != 0x20B) { error = "not a PE32+ image"; return false; }

        ulong preferred = BinaryPrimitives.ReadUInt64LittleEndian(file.AsSpan(optional + 24));
        int sizeOfImage = BinaryPrimitives.ReadInt32LittleEndian(file.AsSpan(optional + 56));
        int sizeOfHeaders = BinaryPrimitives.ReadInt32LittleEndian(file.AsSpan(optional + 60));
        int numberOfDirectories = BinaryPrimitives.ReadInt32LittleEndian(file.AsSpan(optional + 108));

        // Data directories start at optional + 112; index 5 is the base relocations.
        bool hasRelocations = false;
        if (numberOfDirectories > 5)
        {
            int relocRva = BinaryPrimitives.ReadInt32LittleEndian(file.AsSpan(optional + 112 + 5 * 8));
            int relocSize = BinaryPrimitives.ReadInt32LittleEndian(file.AsSpan(optional + 112 + 5 * 8 + 4));
            hasRelocations = relocRva != 0 && relocSize != 0;
        }

        var sections = new List<(int, int, int, int)>();
        int sectionTable = optional + optionalSize;
        for (int i = 0; i < sectionCount; i++)
        {
            int entry = sectionTable + i * 40;
            if (entry + 40 > file.Length) { error = "a section header runs past the file"; return false; }
            sections.Add((
                BinaryPrimitives.ReadInt32LittleEndian(file.AsSpan(entry + 12)),  // virtual address
                BinaryPrimitives.ReadInt32LittleEndian(file.AsSpan(entry + 8)),   // virtual size
                BinaryPrimitives.ReadInt32LittleEndian(file.AsSpan(entry + 20)),  // raw pointer
                BinaryPrimitives.ReadInt32LittleEndian(file.AsSpan(entry + 16)))); // raw size
        }

        image = new PeImage
        {
            Machine = machine,
            IsDll = (characteristics & 0x2000) != 0,
            HasRelocations = hasRelocations,
            PreferredBase = unchecked((IntPtr)(long)preferred),
            SizeOfImage = sizeOfImage,
            SizeOfHeaders = sizeOfHeaders,
            Sections = sections,
        };
        return true;
    }

    /// <summary>
    /// Lays the file out the way the loader would: headers and each section at its
    /// virtual address, zero-filled to the virtual size. `baseAddress` is passed in
    /// so the caller can pass it to the stub verbatim; nothing here depends on the
    /// value, which is the point — the image is reloc-free by contract.
    /// </summary>
    public byte[] BuildMappedImage(byte[] file, IntPtr baseAddress)
    {
        _ = baseAddress;
        byte[] mapped = new byte[SizeOfImage];
        Array.Copy(file, 0, mapped, 0, Math.Min(SizeOfHeaders, Math.Min(file.Length, SizeOfImage)));

        foreach ((int virtualAddress, int virtualSize, int rawPointer, int rawSize) in Sections)
        {
            if (virtualAddress <= 0 || virtualAddress >= SizeOfImage) continue;
            int copy = Math.Min(Math.Min(rawSize, virtualSize), Math.Min(file.Length - rawPointer, SizeOfImage - virtualAddress));
            if (copy <= 0) continue;
            Array.Copy(file, rawPointer, mapped, virtualAddress, copy);
        }
        return mapped;
    }
}
