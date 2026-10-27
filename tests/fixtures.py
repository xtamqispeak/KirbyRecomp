"""Original synthetic ARM64 test programs; no commercial game data."""
import hashlib
from pathlib import Path
import struct

import lz4.block

TITLE_ID = 0x0100152000022000
PROGRAM = [
    0x8B010000,  # add x0, x0, x1
    0xF100281F,  # cmp x0, #10
    0x5400004D,  # b.le +8
    0x91001C00,  # add x0, x0, #7
    0xF9400062,  # ldr x2, [x3]
    0x8B020000,  # add x0, x0, x2
    0xF9000060,  # str x0, [x3]
    0x1E212800,  # fadd s0, s0, s1
    0xD65F03C0,  # ret
]


def write_nso(path, compressed=False, imports=(), export='entry', bits=64):
    text = struct.pack('<4I', 0x14000004, 0x1000, 0xD503201F, 0xD503201F)
    text += struct.pack(f'<{len(PROGRAM)}I', *PROGRAM)
    ro = bytearray(0x400)
    # MOD0: dynamic data, BSS, empty EH table and a module-object pointer.
    struct.pack_into('<4s6i', ro, 0, b'MOD0', 0x1000, 0x1100, 0x2000, -0x1000, -0x1000, 0x1080)
    strings = bytearray(b'\0')
    symbols = [(0, 0, 0, 0, 0, 0)]
    name_offset = len(strings)
    strings += export.encode() + b'\0'
    symbols.append((name_offset, 0x12, 0, 1, 0x10, len(PROGRAM) * 4))
    for name, weak in imports:
        name_offset = len(strings)
        strings += name.encode() + b'\0'
        symbols.append((name_offset, 0x22 if weak else 0x12, 0, 0, 0, 0))
    struct.pack_into('<II', ro, 0x40, 1, len(symbols))
    for i, symbol in enumerate(symbols):
        if bits == 64:
            struct.pack_into('<IBBHQQ', ro, 0x80 + i * 24, *symbol)
        else:
            name, info, other, shndx, value, size = symbol
            struct.pack_into('<IIIBBH', ro, 0x80 + i * 16, name, value, size, info, other, shndx)
    ro[0x200:0x200 + len(strings)] = strings
    data = bytearray(0x100)
    for i, entry in enumerate(((4, 0x1040), (5, 0x1200), (6, 0x1080), (10, len(strings)), (0, 0))):
        struct.pack_into('<QQ' if bits == 64 else '<II', data, i * (16 if bits == 64 else 8), *entry)
    header = bytearray(0x100)
    header[:4] = b'NSO0'
    struct.pack_into('<I', header, 0xC, 0x38 | (7 if compressed else 0))
    struct.pack_into('<I', header, 0x3C, 0xF00)
    # A deliberately trailing-zero build ID tests that it is not shortened.
    header[0x40:0x60] = hashlib.sha256(export.encode()).digest()[:28] + b'\0' * 4
    chunks, offset = [], 0x100
    for i, (mem_off, blob) in enumerate(((0, text), (0x1000, ro), (0x2000, data))):
        blob = bytes(blob)
        stored = lz4.block.compress(blob, store_size=False) if compressed else blob
        struct.pack_into('<III', header, 0x10 + i * 16, offset, mem_off, len(blob))
        struct.pack_into('<I', header, 0x60 + i * 4, len(stored))
        header[0xA0 + i * 32:0xC0 + i * 32] = hashlib.sha256(blob).digest()
        chunks.append(stored)
        offset += len(stored)
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(header + b''.join(chunks))
    return path


def write_npdm(path, title_id=TITLE_ID, bits=64):
    meta = bytearray(0xC0)
    meta[:4] = b'META'
    meta[0xC], meta[0xE] = int(bits == 64), 44
    struct.pack_into('<I', meta, 0x1C, 0x100000)
    meta[0x20:0x29] = b'Synthetic'
    struct.pack_into('<II', meta, 0x70, 0x80, 0x40)
    meta[0x80:0x84] = b'ACI0'
    struct.pack_into('<Q', meta, 0x90, title_id)
    Path(path).write_bytes(meta)


def write_game(path, compressed=False, extra_modules=False, title_id=TITLE_ID, bits=64):
    path = Path(path)
    write_nso(path / 'exefs' / 'main', compressed, imports=(('missing_test_symbol', False), ('weak_test_symbol', True)), bits=bits)
    write_npdm(path / 'exefs' / 'main.npdm', title_id, bits)
    (path / 'romfs').mkdir()
    if extra_modules:
        write_nso(path / 'exefs' / 'subsdk0', compressed, export='sub_entry')
        write_nso(path / 'exefs' / 'sdk', compressed, export='sdk_entry')
    return path


def write_pfs0(path, files):
    strings, entries, offset = bytearray(), bytearray(), 0
    for name, data in files:
        entries += struct.pack('<QQII', offset, len(data), len(strings), 0)
        strings += name.encode() + b'\0'
        offset += len(data)
    Path(path).write_bytes(struct.pack('<4sIII', b'PFS0', len(files), len(strings), 0) + entries + strings + b''.join(x[1] for x in files))
