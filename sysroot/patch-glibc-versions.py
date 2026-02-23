#!/usr/bin/env python3
"""
Patch ELF binaries to replace old GLIBC version tags with GLIBC_2.4.

ARM glibc starts at GLIBC_2.4 -- there are no 2.0/2.1/2.1.3/2.3.2 versions
for armhf. This patches both:
  1. The version strings in .dynstr
  2. The version hashes in .gnu.version_r (SHT_GNU_verneed)

Without fixing the hashes, the dynamic linker fails to match even though
the string reads "GLIBC_2.4" because the hash still corresponds to the old name.
"""
import sys
import struct

def elfhash(name):
    """Compute the ELF hash of a string (same algorithm as dl-hash.h)."""
    h = 0
    for c in name.encode('ascii') if isinstance(name, str) else name:
        h = ((h << 4) + c) & 0xffffffff
        g = h & 0xf0000000
        if g:
            h ^= g >> 24
        h &= ~g & 0xffffffff
    return h

# Map old version -> new version, with precomputed hashes
VERSION_MAP = {
    'GLIBC_2.0':   'GLIBC_2.4',
    'GLIBC_2.1':   'GLIBC_2.4',
    'GLIBC_2.1.3': 'GLIBC_2.4',
    'GLIBC_2.3':   'GLIBC_2.4',
    'GLIBC_2.3.2': 'GLIBC_2.4',
}

OLD_HASHES = {elfhash(k): k for k in VERSION_MAP}
NEW_HASH = elfhash('GLIBC_2.4')

def find_section(data, name_bytes):
    """Find an ELF section by name. Returns (offset, size) or None."""
    # ELF32 header
    ei_class = data[4]
    if ei_class == 1:  # 32-bit
        e_shoff = struct.unpack_from('<I', data, 0x20)[0]
        e_shentsize = struct.unpack_from('<H', data, 0x2e)[0]
        e_shnum = struct.unpack_from('<H', data, 0x30)[0]
        e_shstrndx = struct.unpack_from('<H', data, 0x32)[0]
    elif ei_class == 2:  # 64-bit
        e_shoff = struct.unpack_from('<Q', data, 0x28)[0]
        e_shentsize = struct.unpack_from('<H', data, 0x3a)[0]
        e_shnum = struct.unpack_from('<H', data, 0x3c)[0]
        e_shstrndx = struct.unpack_from('<H', data, 0x3e)[0]
    else:
        return None

    # Read section header string table
    if ei_class == 1:
        shstr_off = struct.unpack_from('<I', data, e_shoff + e_shstrndx * e_shentsize + 0x10)[0]
    else:
        shstr_off = struct.unpack_from('<Q', data, e_shoff + e_shstrndx * e_shentsize + 0x18)[0]

    # Search sections
    for i in range(e_shnum):
        sh = e_shoff + i * e_shentsize
        if ei_class == 1:
            sh_name_idx = struct.unpack_from('<I', data, sh)[0]
            sh_type = struct.unpack_from('<I', data, sh + 4)[0]
            sh_offset = struct.unpack_from('<I', data, sh + 0x10)[0]
            sh_size = struct.unpack_from('<I', data, sh + 0x14)[0]
        else:
            sh_name_idx = struct.unpack_from('<I', data, sh)[0]
            sh_type = struct.unpack_from('<I', data, sh + 4)[0]
            sh_offset = struct.unpack_from('<Q', data, sh + 0x18)[0]
            sh_size = struct.unpack_from('<Q', data, sh + 0x20)[0]

        # Get section name from shstrtab
        name_start = shstr_off + sh_name_idx
        name_end = data.index(b'\x00', name_start)
        sec_name = bytes(data[name_start:name_end])

        if sec_name == name_bytes:
            return sh_offset, sh_size, sh_type

    return None

def patch_verneed_hashes(data):
    """Patch vna_hash values in .gnu.version_r section."""
    result = find_section(data, b'.gnu.version_r')
    if result is None:
        print("  WARNING: .gnu.version_r section not found")
        return 0

    sec_off, sec_size, sh_type = result
    # SHT_GNU_verneed = 0x6ffffffe
    count = 0

    # Parse Verneed entries (linked list via vn_next)
    pos = sec_off
    while True:
        # Elf32_Verneed: vn_version(2), vn_cnt(2), vn_file(4), vn_aux(4), vn_next(4)
        vn_version, vn_cnt, vn_file, vn_aux, vn_next = struct.unpack_from('<HHIII', data, pos)

        # Parse Vernaux entries (linked list via vna_next)
        aux_pos = pos + vn_aux
        for _ in range(vn_cnt):
            # Elf32_Vernaux: vna_hash(4), vna_flags(2), vna_other(2), vna_name(4), vna_next(4)
            vna_hash, vna_flags, vna_other, vna_name, vna_next = struct.unpack_from('<IHHII', data, aux_pos)

            if vna_hash in OLD_HASHES:
                old_name = OLD_HASHES[vna_hash]
                print(f"  Patching hash for {old_name}: 0x{vna_hash:08x} -> 0x{NEW_HASH:08x} at offset 0x{aux_pos:x}")
                struct.pack_into('<I', data, aux_pos, NEW_HASH)
                count += 1

            if vna_next == 0:
                break
            aux_pos += vna_next

        if vn_next == 0:
            break
        pos += vn_next

    return count

def patch_dynstr_versions(data):
    """Patch version strings in .dynstr section."""
    replacements = [
        # (old bytes, new bytes) - must be same length
        # Longer strings first to avoid partial matches
        (b'GLIBC_2.1.3\x00', b'GLIBC_2.4\x00\x00\x00'),
        (b'GLIBC_2.3.2\x00', b'GLIBC_2.4\x00\x00\x00'),
        # Do GLIBC_2.3\x00 before GLIBC_2.0 but after GLIBC_2.3.2
        (b'GLIBC_2.3\x00', b'GLIBC_2.4\x00'),
        (b'GLIBC_2.0\x00', b'GLIBC_2.4\x00'),
        # Do GLIBC_2.1\x00 last since it's a prefix of GLIBC_2.1.3
        (b'GLIBC_2.1\x00', b'GLIBC_2.4\x00'),
    ]

    count = 0
    for old, new in replacements:
        assert len(old) == len(new), f"Length mismatch: {old!r} vs {new!r}"
        pos = 0
        while True:
            idx = data.find(old, pos)
            if idx == -1:
                break
            data[idx:idx+len(new)] = new
            count += 1
            pos = idx + len(new)
            print(f"  Patched string {old!r} -> {new!r} at offset 0x{idx:x}")

    return count

def patch_file(path):
    with open(path, 'rb') as f:
        data = bytearray(f.read())

    # Verify it's an ELF
    if data[:4] != b'\x7fELF':
        print(f"  ERROR: {path} is not an ELF file")
        return

    str_count = patch_dynstr_versions(data)
    hash_count = patch_verneed_hashes(data)
    total = str_count + hash_count

    if total > 0:
        with open(path, 'wb') as f:
            f.write(data)
        print(f"  {str_count} string patches + {hash_count} hash patches applied to {path}")
    else:
        print(f"  No patches needed for {path}")

if __name__ == '__main__':
    for path in sys.argv[1:]:
        print(f"Patching {path}...")
        patch_file(path)
