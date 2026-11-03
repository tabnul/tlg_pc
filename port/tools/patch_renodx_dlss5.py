"""Patch renodx-dlss5.addon64 so it runs in Vulkan games (see docs/dlss5.md).

The add-on's queue completion tracker detours D3D12 ExecuteCommandLists through the vtable of
whatever native queue ReShade reports; in a Vulkan game that is an NVIDIA driver function, which
crashes on the first DLSS 5 frame. This makes the tracker installer return false.
Tested with file version 0.2026.917.1432 (6.5.3). Keeps a .orig copy.
"""
import shutil
import struct
import sys

RVA = 0x34370
EXPECTED = bytes.fromhex("48895c2410")  # mov [rsp+10h], rbx
PATCH = bytes.fromhex("32c0c3")  # xor al, al ; ret


def file_offset(data, rva):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    sections = struct.unpack_from("<H", data, pe + 6)[0]
    table = pe + 24 + struct.unpack_from("<H", data, pe + 20)[0]
    for i in range(sections):
        size, address, _, raw = struct.unpack_from("<IIII", data, table + i * 40 + 8)
        if address <= rva < address + size:
            return raw + rva - address
    raise SystemExit("address not in any section")


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "renodx-dlss5.addon64"
    data = bytearray(open(path, "rb").read())
    offset = file_offset(data, RVA)
    if data[offset:offset + 3] == PATCH:
        print("already patched")
        return
    if data[offset:offset + 5] != EXPECTED:
        raise SystemExit("unexpected bytes: different add-on version, not patched")
    shutil.copyfile(path, path + ".orig")
    data[offset:offset + 3] = PATCH
    open(path, "wb").write(data)
    print("patched; original kept as", path + ".orig")


if __name__ == "__main__":
    main()
