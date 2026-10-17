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

    print("=== Call site bytes ===")
    for va in sites:
        off = va_to_offset(sections, va)
        if off is None:
            print(f'0x{va:X}: NO MAPPING')
            continue
        f.seek(off)
        data = f.read(5)
        hexs = ' '.join(f'{b:02X}' for b in data)
        if data[0] == 0xE8:
            rel = struct.unpack_from('<i', data, 1)[0]
            target = va + 5 + rel
            print(f'0x{va:X}: [{hexs}] -> 0x{target:X}')
        else:
            print(f'0x{va:X}: [{hexs}] NOT A CALL')

    print()
    print('=== Context around each call site ===')
    for va in sites:
        start = va - 16
        off = va_to_offset(sections, start)
        if off is None:
            print(f'--- 0x{va:X} --- NO MAPPING')
            continue
        f.seek(off)
        data = f.read(40)
        print(f'--- 0x{va:X} ---')
        for i in range(0, len(data), 16):
            addr = start + i
            chunk = data[i:i+16]
            hexs = ' '.join(f'{b:02X}' for b in chunk)
            print(f'  0x{addr:X}: {hexs}')