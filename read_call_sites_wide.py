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

    # All 9 call sites in FUN_140787a84 that call FUN_140764210
    sites = [
        0x14078849a, 0x1407884ad, 0x14078854f, 0x140788572,
        0x1407885d0, 0x14078864d, 0x140788678, 0x1407886a9,
        0x1407886bd
    ]

    # Read wider context (128 bytes before each call) to find collision type checks
    print("=== Wide context (128 bytes before each call) ===")
    for va in sites:
        start = va - 128
        off = va_to_offset(sections, start)
        if off is None:
            print(f'--- 0x{va:X} --- NO MAPPING')
            continue
        f.seek(off)
        data = f.read(128 + 5)  # 128 bytes before + 5 byte call
        print(f'\n--- 0x{va:X} (call at end) ---')
        for i in range(0, len(data), 16):
            addr = start + i
            chunk = data[i:i+16]
            hexs = ' '.join(f'{b:02X}' for b in chunk)
            # Mark the call instruction
            marker = ' <-- CALL' if addr == va else ''
            print(f'  0x{addr:X}: {hexs}{marker}')