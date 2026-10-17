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

    # Dump from 0x1407884C0 (just before 0x12 cmp) through 0x1407885A0 (into 0x17 block)
    # This covers the entire 0x12/0x1E guard-hit block
    dump_start = 0x1407884C0
    dump_end = 0x1407885A0
    off = va_to_offset(sections, dump_start)
    f.seek(off)
    data = f.read(dump_end - dump_start)

    print("=== Guard-hit block (0x12/0x1E) hex dump ===")
    print(f"   0x1407884C5: cmp [r13+0x50], 0x12")
    print(f"   0x1407884CC: cmp [r13+0x50], 0x1E")
    print(f"   0x140788597: cmp [r13+0x50], 0x17 (next block)")
    print()

    call_sites_in_range = []
    for i in range(0, len(data), 16):
        addr = dump_start + i
        chunk = data[i:i+16]
        hexs = ' '.join(f'{b:02X}' for b in chunk)
        marker = ''
        # Check if any E8 call in this row targets FUN_140764210
        for j in range(min(12, len(chunk))):
            if chunk[j] == 0xE8 and j + 4 < len(chunk):
                rel = struct.unpack_from('<i', chunk, j)[0]
                target = addr + j + 5 + rel
                if target == 0x140764210:
                    marker = f' <-- CALL FUN_140764210 at 0x{addr+j:X}'
                    call_sites_in_range.append(addr + j)
        print(f"  0x{addr:X}: {hexs}{marker}")

    print()
    print(f"=== FUN_140764210 calls in guard-hit block: {[hex(x) for x in call_sites_in_range]} ===")

    # Now also check: in the decompiled 0x12/0x1E block, the FIRST call is
    # FUN_140764210(param_1, opponent_state) -- THIS IS THE BUG
    # The SECOND call is FUN_140764210(plVar14, self_state) -- also suspect
    # We need to NOP both to prevent cross-contamination of action states

    # Also dump the 0x0C block calls for comparison
    print()
    print("=== 0x0C block (for reference) ===")
    dump_start2 = 0x14078841A
    dump_end2 = 0x1407884C5
    off2 = va_to_offset(sections, dump_start2)
    f.seek(off2)
    data2 = f.read(dump_end2 - dump_start2)
    for i in range(0, len(data2), 16):
        addr = dump_start2 + i
        chunk = data2[i:i+16]
        hexs = ' '.join(f'{b:02X}' for b in chunk)
        marker = ''
        for j in range(min(12, len(chunk))):
            if chunk[j] == 0xE8 and j + 4 < len(chunk):
                rel = struct.unpack_from('<i', chunk, j)[0]
                target = addr + j + 5 + rel
                if target == 0x140764210:
                    marker = f' <-- CALL FUN_140764210 at 0x{addr+j:X}'
        print(f"  0x{addr:X}: {hexs}{marker}")