import struct
import os

dmp_path = r'C:\Users\35295\Desktop\openppp2\openppp2-windows-x64-debug_20260702_1622-20260703-003659.dmp'
crash_addr = 0x7FF7FC2BEA29

with open(dmp_path, 'rb') as f:
    header = f.read(32)
    sig, ver, num_streams, stream_rva, _, _, flags = struct.unpack_from('<IIIIIIQ', header, 0)
    print(f'Signature: {hex(sig)}, NumStreams: {num_streams}, StreamRVA: {hex(stream_rva)}')
    
    f.seek(stream_rva)
    mem64_rva = None
    for i in range(num_streams):
        sd = f.read(12)
        stype, ssize, srva = struct.unpack_from('<III', sd, 0)
        if stype == 9:
            mem64_rva = srva
            print(f'Memory64ListStream at RVA {hex(mem64_rva)}, size={ssize}')
            break
    
    f.seek(mem64_rva)
    num_ranges = struct.unpack_from('<Q', f.read(8))[0]
    print(f'Num ranges: {num_ranges}')
    
    descriptors_rva = mem64_rva + 8
    data_start_rva = descriptors_rva + num_ranges * 16
    
    # Dump first 10 descriptors raw bytes
    f.seek(descriptors_rva)
    print(f'\nFirst 10 descriptors (16 bytes each):')
    for i in range(10):
        raw = f.read(16)
        start, size = struct.unpack_from('<QQ', raw, 0)
        print(f'  [{i}] start={hex(start)} size={hex(size)} size_dec={size}')
    
    # Check if crash_addr is in ppp.exe's expected range
    # ppp.exe base should be around 0x7FF7FB910000
    print(f'\nSearching for range containing ppp.exe base 0x7FF7FB910000...')
    f.seek(descriptors_rva)
    for i in range(num_ranges):
        raw = f.read(16)
        start, size = struct.unpack_from('<QQ', raw, 0)
        if start <= 0x7FF7FB910000 < start + size:
            print(f'  Range {i}: start={hex(start)} size={hex(size)} ({size} bytes)')
            print(f'    End={hex(start + size)}')
    
    # Find range that CONTAINS the crash addr by checking start <= crash < start+size
    print(f'\nSearching for crash addr {hex(crash_addr)} in ranges...')
    f.seek(descriptors_rva)
    found = False
    for i in range(num_ranges):
        raw = f.read(16)
        start, size = struct.unpack_from('<QQ', raw, 0)
        if start <= crash_addr < start + size:
            print(f'  *** FOUND range {i}: start={hex(start)} size={hex(size)}')
            if size > 0x100000000:  # > 4GB
                print(f'      WARNING: size too large ({size}), likely corrupted descriptor!')
            else:
                print(f'      End={hex(start + size)}')
                found = True
                break
    
    if not found:
        print(f'  Crash addr not found by direct search!')
        print(f'  The crash address might be in a WRITE section or data section')
        print(f'  Let me check what ranges overlap with ppp.exe memory...')
        
        f.seek(descriptors_rva)
        for i in range(num_ranges):
            raw = f.read(16)
            start, size = struct.unpack_from('<QQ', raw, 0)
            # Check if range overlaps with ppp.exe: base 0x7FF7FB910000 to base+0x16B000 (23MB)
            ppp_base = 0x7FF7FB910000
            ppp_end = ppp_base + 0x2000000  # 32MB to be safe
            if start < ppp_end and start + size > ppp_base:
                print(f'  Overlapping range {i}: start={hex(start)} size={hex(size)}')
                if size < 100000000:  # sane size
                    print(f'    Contains crash: {start <= crash_addr < start + size}')

    # Print ranges sorted by start address
    print(f'\nReading ALL ranges sorted by start...')
    f.seek(descriptors_rva)
    ranges = []
    for i in range(num_ranges):
        raw = f.read(16)
        start, size = struct.unpack_from('<QQ', raw, 0)
        ranges.append((start, size, i))
    
    ranges.sort()
    
    # Find the range that most precisely contains the crash address
    for start, size, idx in ranges:
        if start <= crash_addr < start + size:
            print(f'  SORTED-FOUND range {idx}: start={hex(start)} size={hex(size)} ({size} bytes)')
            if size < 100000000:
                print(f'    VALID RANGE - reading code bytes...')
                # Calculate cumulative data before this range
                cum = 0
                f.seek(descriptors_rva)
                for j in range(idx):
                    r = f.read(16)
                    s, sz = struct.unpack_from('<QQ', r, 0)
                    cum += sz
                
                offset_in_range = crash_addr - start
                file_offset = data_start_rva + cum + offset_in_range
                file_size = os.path.getsize(dmp_path)
                print(f'    Cumulative data before: {hex(cum)} ({cum})')
                print(f'    Offset in range: {hex(offset_in_range)}')
                print(f'    Data start RVA: {hex(data_start_rva)}')
                print(f'    File offset: {hex(file_offset)} ({file_offset})')
                print(f'    File size: {hex(file_size)} ({file_size})')
                
                if file_offset + 64 <= file_size:
                    f.seek(file_offset)
                    code = f.read(64)
                    print(f'    CODE bytes: {code.hex()}')
                    print(f'    Hex: {" ".join(f"{b:02x}" for b in code)}')
                else:
                    print(f'    OFF BY: {file_offset - file_size}')
            else:
                print(f'    SIZE TOO LARGE - skipping')
            break
    else:
        print(f'  Crash addr NOT in any range (sorted search)')
        # Check if it's in range 161 (the one found earlier)
        r161_start, r161_size = ranges[161][:2]
        print(f'  Range 161: start={hex(r161_start)} size={hex(r161_size)}')
        print(f'  Crash in range 161: {r161_start <= crash_addr < r161_start + r161_size}')
