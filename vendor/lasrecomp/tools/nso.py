"""Bounded NSO reader adapted from LASRecomp (see ../LICENSE).

Format reference: https://switchbrew.org/wiki/NSO0
The upstream public attributes are retained for gen.py and analyze.py.
"""
import hashlib
import struct
from pathlib import Path

import lz4.block

DT_NULL, DT_NEEDED, DT_PLTRELSZ, DT_HASH, DT_STRTAB, DT_SYMTAB, DT_RELA, DT_RELASZ = 0, 1, 2, 4, 5, 6, 7, 8
DT_STRSZ, DT_REL, DT_RELSZ, DT_JMPREL, DT_RELACOUNT = 10, 17, 18, 23, 0x6FFFFFF9
R_AARCH64_ABS64, R_AARCH64_GLOB_DAT, R_AARCH64_JUMP_SLOT, R_AARCH64_RELATIVE = 257, 1025, 1026, 1027
MAX_IMAGE = 512 * 1024 * 1024


class Nso:
    def __init__(self, path, bits=64):
        if bits not in (32, 64):
            raise ValueError('Expected a 32-bit or 64-bit NSO ABI')
        self.bits = bits
        path = Path(path)
        if path.stat().st_size > MAX_IMAGE:
            raise ValueError(f'{path.name}: NSO file exceeds 512 MiB limit')
        raw = path.read_bytes()
        if len(raw) < 0x100 or raw[:4] != b'NSO0':
            raise ValueError(f'{path.name}: expected a complete NSO0 header')
        flags = struct.unpack_from('<I', raw, 0xC)[0]
        if flags & 0x80:
            raise ValueError('Zbic compression is not supported')
        self.build_id = raw[0x40:0x60].hex()
        self.bss_size = struct.unpack_from('<I', raw, 0x3C)[0]
        segs = []
        previous_end = 0
        for i, name in enumerate(('text', 'rodata', 'data')):
            file_off, mem_off, dec_size = struct.unpack_from('<III', raw, 0x10 + i * 0x10)
            stored_size = struct.unpack_from('<I', raw, 0x60 + i * 4)[0] if flags & (1 << i) else dec_size
            if mem_off < previous_end or mem_off + dec_size > MAX_IMAGE:
                raise ValueError(f'{name}: overlapping or oversized memory range')
            if file_off + stored_size > len(raw) or (stored_size and file_off < 0x100):
                raise ValueError(f'{name}: segment extends outside NSO file')
            data = raw[file_off:file_off + stored_size]
            if flags & (1 << i) and dec_size:
                try:
                    data = lz4.block.decompress(data, uncompressed_size=dec_size)
                except lz4.block.LZ4BlockError as e:
                    raise ValueError(f'{name}: invalid LZ4 segment') from e
            if len(data) != dec_size:
                raise ValueError(f'{name}: decompressed size mismatch')
            if flags & (1 << (i + 3)):
                expected = raw[0xA0 + i * 32:0xC0 + i * 32]
                if hashlib.sha256(data).digest() != expected:
                    raise ValueError(f'{name}: SHA-256 mismatch')
            segs.append((mem_off, data))
            previous_end = mem_off + dec_size
        if not segs[0][1] or len(segs[0][1]) % 4:
            raise ValueError('text segment must contain whole ARM64 instructions')
        self.text, self.rodata, self.data = [(off, len(data)) for off, data in segs]
        end = previous_end + self.bss_size
        self.size = (end + 0xFFF) & ~0xFFF
        if self.size > MAX_IMAGE:
            raise ValueError('NSO image including BSS exceeds 512 MiB limit')
        self.image = bytearray(self.size)
        for off, data in segs:
            self.image[off:off + len(data)] = data
        self._dynsym_size = struct.unpack_from('<I', raw, 0x9C)[0]
        self._parse_mod0()

    def check(self, off, size):
        if off < 0 or size < 0 or off + size > len(self.image):
            raise ValueError(f'NSO memory range {off:#x}+{size:#x} is out of bounds')

    def u32(self, off):
        self.check(off, 4)
        return struct.unpack_from('<I', self.image, off)[0]

    def u64(self, off):
        self.check(off, 8)
        return struct.unpack_from('<Q', self.image, off)[0]

    def _parse_mod0(self):
        mod0 = self.u32(self.text[0] + 4)
        self.check(mod0, 28)
        if self.image[mod0:mod0 + 4] != b'MOD0':
            raise ValueError('NSO has no valid MOD0 record')
        offsets = [mod0 + struct.unpack_from('<i', self.image, mod0 + 4 + i * 4)[0] for i in range(6)]
        self.dynamic, self.bss_start, self.bss_end, self.eh_start, self.eh_end, self.module_obj = offsets
        if not 0 <= self.eh_start <= self.eh_end <= len(self.image):
            raise ValueError('invalid exception-header range')
        if self.eh_start == self.eh_end:
            self.eh_start = self.eh_end = 0
        dyn, needed = {}, []
        off = self.dynamic
        word_size = self.bits // 8
        read_word = self.u64 if self.bits == 64 else self.u32
        while True:
            self.check(off, word_size * 2)
            tag, val = read_word(off), read_word(off + word_size)
            off += word_size * 2
            if tag == DT_NULL:
                break
            if tag == DT_NEEDED:
                needed.append(val)
            dyn.setdefault(tag, val)
        self.dyn = dyn
        self.strtab = dyn.get(DT_STRTAB, 0)
        self.strsize = dyn.get(DT_STRSZ, 0)
        if self.strsize:
            self.check(self.strtab, self.strsize)
        self.needed = [self.cstr(self.strtab + n) for n in needed]
        self.syms = []
        if DT_SYMTAB in dyn:
            sym_size = 24 if self.bits == 64 else 16
            nchain = self.u32(dyn[DT_HASH] + 4) if DT_HASH in dyn else self._dynsym_size // sym_size
            if not nchain:
                raise ValueError('cannot determine dynamic symbol count')
            st = dyn[DT_SYMTAB]
            self.check(st, nchain * sym_size)
            for i in range(nchain):
                o = st + i * sym_size
                if self.bits == 64:
                    name, info, other, shndx = struct.unpack_from('<IBBH', self.image, o)
                    value, size = struct.unpack_from('<QQ', self.image, o + 8)
                else:
                    name, value, size, info, other, shndx = struct.unpack_from('<IIIBBH', self.image, o)
                self.syms.append((self.cstr(self.strtab + name), info, shndx, value, size))
        self.relocs = []
        rela_size = 24 if self.bits == 64 else 12
        for base_tag, size_tag, is_rel in ((DT_RELA, DT_RELASZ, False), (DT_REL, DT_RELSZ, True),
                                          (DT_JMPREL, DT_PLTRELSZ, dyn.get(20) == DT_REL)):
            if base_tag not in dyn:
                continue
            size = dyn.get(size_tag, 0)
            stride = word_size * 2 if is_rel else rela_size
            if size % stride:
                raise ValueError('relocation table size does not match ABI entry size')
            self.check(dyn[base_tag], size)
            for o in range(dyn[base_tag], dyn[base_tag] + size, stride):
                r_off, r_info = read_word(o), read_word(o + word_size)
                self.check(r_off, word_size)
                r_add = read_word(r_off) if is_rel else struct.unpack_from('<q' if self.bits == 64 else '<i', self.image, o + 2 * word_size)[0]
                si, kind = (r_info >> 32, r_info & 0xFFFFFFFF) if self.bits == 64 else (r_info >> 8, r_info & 0xFF)
                relative_type = R_AARCH64_RELATIVE if self.bits == 64 else 23
                if kind != relative_type and si >= len(self.syms):
                    raise ValueError('relocation references an invalid symbol index')
                self.relocs.append((r_off, kind, si, r_add))

    def cstr(self, off):
        self.check(off, 1)
        end_limit = self.strtab + self.strsize if self.strsize else len(self.image)
        if self.strsize and not self.strtab <= off < end_limit:
            raise ValueError('string offset outside dynamic string table')
        end = self.image.find(b'\0', off, end_limit)
        if end < 0:
            raise ValueError('unterminated dynamic string')
        return self.image[off:end].decode('utf-8', 'replace')

    def imports(self):
        return [s for s in self.syms if s[0] and s[2] == 0]

    def exports(self):
        return [s for s in self.syms if s[0] and s[2] != 0]
