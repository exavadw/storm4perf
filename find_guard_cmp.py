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

    # Scan the entire FUN_140787a84 function region for cmp instructions checking collision types
    # Function starts at 0x140787a84, scan through 0x140788700
    start_va = 0x140788400
    end_va = 0x140788700
    off = va_to_offset(sections, start_va)
    size = end_va - start_va
    f.seek(off)
    data = f.read(size)

    # Look for patterns that match collision type checks:
    # 41 83 7D 50 XX = cmp dword [r13+0x50], imm8
    # 41 83 BC 24 XX XX 00 00 YY = cmp dword [r12+XX], imm8
    # Also look for: 83 F8 XX (cmp eax, imm8) after loading the collision type
    print("=== Collision type comparisons in FUN_140787a84 (0x140788400-0x140788700) ===")
    for i in range(len(data)):
        va = start_va + i
        # Pattern: 41 83 7D 50 XX (cmp [r13+0x50], imm8)
        if i + 4 < len(data) and data[i:i+4] == b'\x41\x83\x7d\x50':
            imm = data[i+4]
            print(f"  0x{va:X}: cmp [r13+0x50], 0x{imm:02X}")
        # Pattern: 41 83 BC 24 (cmp [r12+disp32], imm8)
        if i + 7 < len(data) and data[i:i+4] == b'\x41\x83\xbc\x24':
            disp = struct.unpack_from('<I', data, i+4)[0]
            imm = data[i+8] if i+8 < len(data) else 0
            print(f"  0x{va:X}: cmp [r12+0x{disp:X}], 0x{imm:02X}")
        # Pattern: 83 F8 XX (cmp eax, imm8)
        if i + 2 < len(data) and data[i:i+2] == b'\x83\xf8':
            imm = data[i+2]
            print(f"  0x{va:X}: cmp eax, 0x{imm:02X}")
        # Pattern: 83 FE XX (cmp esi, imm8)
        if i + 2 < len(data) and data[i:i+2] == b'\x83\xfe':
            imm = data[i+2]
            print(f"  0x{va:X}: cmp esi, 0x{imm:02X}")
        # Pattern: 83 FF XX (cmp edi, imm8)
        if i + 2 < len(data) and data[i:i+2] == b'\x83\xff':
            imm = data[i+2]
            print(f"  0x{va:X}: cmp edi, 0x{imm:02X}")

    # Now dump the full disassembly-like hex for the region between the 0x0C block and 0x17 block
    # to find where 0x12/0x1E checks are
    print("\n=== Full hex dump 0x140788580-0x1407886D0 (between 0x0C and 0x17 blocks) ===")
    dump_start = 0x140788580
    dump_end = 0x1407886D0
    off = va_to_offset(sections, dump_start)
    f.seek(off)
    data = f.read(dump_end - dump_start)
    for i in range(0, len(data), 16):
        addr = dump_start + i
        chunk = data[i:i+16]
        hexs = ' '.join(f'{b:02X}' for b in chunk)
        print(f"  0x{addr:X}: {hexs}")