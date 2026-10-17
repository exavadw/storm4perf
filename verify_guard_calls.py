import struct

def va_to_offset(sections, va):
    rva = va - 0x140000000
    for name, vaddr, vsize, roffset, rsize in sections:
        if vaddr <= rva < vaddr + vsize:
            return roffset + (rva - vaddr)
    return None

with open('/home/hbarorhp/ghidra/binaries/pc/NSUNS4_bugtest.exe', 'rb') as f:
    e_lfanew = struct.unpack_from('<I', f.read(64), 0x3C)[0]
    f.seek(e_lfanew)
    sig = f.read(4)
    machine, nsec, ts, symoff, nsym, opthdr, chars = struct.unpack_from('<HHIIIHH', f.read(20))
    opt_start = f.tell()
    f.seek(opt_start + 56)
    sec_align = struct.unpack_from('<I', f.read(4))[0]
    f.seek(opt_start + 60)
    file_align = struct.unpack_from('<I', f.read(4))[0]
    f.seek(e_lfanew + 24 + opthdr)
    sections = []
    for i in range(nsec):
        name = f.read(8).rstrip(b'\x00').decode('ascii','replace')
        vsize, vaddr, rsize, roffset = struct.unpack_from('<IIII', f.read(16))
        f.read(16)
        sections.append((name, vaddr, vsize, roffset, rsize))

    # The two calls in the 0x12/0x1E guard-hit block that need NOPping:
    # 0x14078854F: FUN_140764210(param_1, opponent_state) -- BUG: writes opponent state into self
    # 0x140788572: FUN_140764210(plVar14, self_state)     -- BUG: writes self state into opponent
    guard_calls = [0x14078854F, 0x140788572]

    print("=== Verifying guard-hit block calls to NOP ===")
    for va in guard_calls:
        off = va_to_offset(sections, va)
        if off is None:
            print(f"0x{va:X}: NO MAPPING")
            continue
        f.seek(off)
        data = f.read(5)
        hexs = ' '.join(f'{b:02X}' for b in data)
        if data[0] == 0xE8:
            rel = struct.unpack_from('<i', data, 1)[0]
            target = va + 5 + rel
            match = "MATCH" if target == 0x140764210 else f"MISMATCH (target=0x{target:X})"
            print(f"0x{va:X}: [{hexs}] -> 0x{target:X} {match}")
        else:
            print(f"0x{va:X}: [{hexs}] NOT A CALL")

    # Also verify the existing guardfix sites still match
    print("\n=== Existing guardfix sites ===")
    existing = {
        'write': (0x14077E6B6, bytes([0xF3,0x0F,0x11,0x05, 0xFE,0x87,0xEE,0x00])),
        'reset': (0x14077E6E6, bytes([0xF3,0x0F,0x11,0x05, 0xCE,0x87,0xEE,0x00])),
        'call1': (0x1407EBF26, bytes([0xE8, 0xE5, 0x82, 0xF7, 0xFF])),
        'call2': (0x1407EBF4B, bytes([0xE8, 0xC0, 0x82, 0xF7, 0xFF])),
    }
    for label, (va, expected) in existing.items():
        off = va_to_offset(sections, va)
        if off is None:
            print(f"{label}: 0x{va:X} NO MAPPING")
            continue
        f.seek(off)
        data = f.read(len(expected))
        match = "MATCH" if data == expected else f"MISMATCH (got {' '.join(f'{b:02X}' for b in data)})"
        print(f"{label}: 0x{va:X} {match}")