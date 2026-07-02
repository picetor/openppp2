import struct
import os

exe_path = r'C:\Users\35295\Desktop\openppp2\openppp2-windows-x64-debug_20260702_1622.exe'
offset = 0x9AEA29

if not os.path.exists(exe_path):
    print(f'EXE not found at: {exe_path}')
    exit(1)

print(f'Analyzing: {exe_path}')
print(f'File size: {os.path.getsize(exe_path)} bytes')

with open(exe_path, 'rb') as f:
    # Read DOS header
    dos = f.read(64)
    e_lfanew = struct.unpack_from('<I', dos, 0x3C)[0]
    print(f'\ne_lfanew: {hex(e_lfanew)}')
    
    # Read PE signature
    f.seek(e_lfanew)
    sig = f.read(4)
    print(f'PE signature: {sig}')
    
    # Read COFF header
    coff = f.read(20)
    machine, num_sections, time_date, sym_tbl_ptr, num_syms, opt_hdr_sz = struct.unpack_from('<HHIIIH', coff, 0)
    print(f'Machine: {hex(machine)}, Sections: {num_sections}')
    
    # Read optional header
    opt_magic = struct.unpack_from('<H', f.read(2))[0]
    print(f'Optional header magic: {hex(opt_magic)}')
    f.seek(-2, 1)  # seek back
    
    if opt_magic == 0x20b:  # PE32+
        opt = f.read(opt_hdr_sz - 2)
        # image_base is at offset 24 in PE32+ optional header
        image_base = struct.unpack_from('<Q', opt, 24)[0]
        print(f'Image base: {hex(image_base)}')
    else:
        opt = f.read(opt_hdr_sz - 2)
        image_base = struct.unpack_from('<I', opt, 28)[0]
        print(f'Image base (32-bit): {hex(image_base)}')
    
    # Read section table
    f.seek(e_lfanew + 24 + opt_hdr_sz)
    
    print(f'\nSection table:')
    for i in range(num_sections):
        sec_data = f.read(40)
        name = sec_data[:8].decode('ascii', errors='replace').rstrip('\0')
        vsize, vrva, rsize, roff, _, _, _, _, characteristics = struct.unpack_from('<IIIIIIHHI', sec_data, 8)
        print(f'  [{i}] {name}: VA={hex(vrva)} VSize={hex(vsize)} RawOff={hex(roff)} RawSize={hex(rsize)}')
        
        # Check if crash offset is in this section
        va_start = image_base + vrva
        va_end = va_start + vsize
        crash_rva = offset  # relative to image base
        if vrva <= crash_rva < vrva + vsize:
            print(f'        *** CRASH OFFSET IN THIS SECTION! ***')
            section_offset_in_file = roff + (crash_rva - vrva)
            print(f'        File offset: {hex(section_offset_in_file)}')
            f.seek(section_offset_in_file)
            code = f.read(min(64, rsize - (crash_rva - vrva)))
            print(f'        Code bytes: {code.hex()}')
            print(f'        Hex: {" ".join(f"{b:02x}" for b in code)}')
