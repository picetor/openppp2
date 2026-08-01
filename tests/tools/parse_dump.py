import struct
import os

dmp_path = r'C:\Users\35295\Desktop\openppp2\openppp2-windows-x64-debug_20260702_1622-20260703-003659.dmp'
crash_addr = 0x7FF7FC2BEA29

with open(dmp_path, 'rb') as f:
    header = f.read(32)
    sig, ver, num_streams, stream_rva, _, _, flags = struct.unpack_from('<IIIIIIQ', header, 0)
    print(f'Signature: {hex(sig)}, Version: {hex(ver)}, NumStreams: {num_streams}, StreamRVA: {hex(stream_rva)}')
    
    # Find Memory64ListStream (type 9)
    f.seek(stream_rva)
    mem64_rva = None
    for i in range(num_streams):
        sd = f.read(12)
        stype, ssize, srva = struct.unpack_from('<III', sd, 0)
        if stype == 9:
            mem64_rva = srva
            mem64_size = ssize
            print(f'Found Memory64ListStream at RVA {hex(mem64_rva)}, size={mem64_size}')
            break
    
    if mem64_rva is None:
        print('Memory64ListStream not found!')
        exit(1)
    
    # Read Memory64ListStream
    f.seek(mem64_rva)
    num_ranges = struct.unpack_from('<Q', f.read(8))[0]
    print(f'Number of memory ranges: {num_ranges}')
    
    descriptors_rva = mem64_rva + 8
    data_start_rva = descriptors_rva + num_ranges * 16
    print(f'Descriptors at RVA {hex(descriptors_rva)}')
    print(f'Data starts at RVA {hex(data_start_rva)}')
    
    # Read all descriptors and find the range containing crash_addr
    f.seek(descriptors_rva)
    found_range_idx = -1
    cumulative_data = 0
    
    for idx in range(num_ranges):
        range_start, range_size = struct.unpack_from('<QQ', f.read(16))
        
        if range_start <= crash_addr < range_start + range_size:
            found_range_idx = idx
            offset_in_range = crash_addr - range_start
            file_offset = data_start_rva + cumulative_data + offset_in_range
            
            print(f'\nFOUND range {idx}: start={hex(range_start)}, size={hex(range_size)} ({range_size} bytes)')
            print(f'  End={hex(range_start + range_size)}')
            print(f'  Offset in range: {hex(offset_in_range)}')
            print(f'  Cumulative data before: {hex(cumulative_data)} ({cumulative_data})')
            print(f'  Data file offset: {hex(file_offset)} ({file_offset})')
            
            file_size = os.path.getsize(dmp_path)
            print(f'  File size: {hex(file_size)} ({file_size} bytes)')
            
            if file_offset + 64 <= file_size:
                f.seek(file_offset)
                code_bytes = f.read(64)
                print(f'  Code bytes at crash: {code_bytes.hex()}')
                print('  Hex:', ' '.join(f'{b:02x}' for b in code_bytes))
                
                # Try basic x86_64 instruction length detection
                # Just show first bytes as potential instruction
                if len(code_bytes) > 0:
                    first_byte = code_bytes[0]
                    print(f'  First byte: {hex(first_byte)}')
                    # Common opcodes:
                    # 0x48 = REX.W prefix
                    # 0x8B = MOV reg, r/m
                    # 0x89 = MOV r/m, reg
                    # 0xFF = INC/DEC/CALL/JMP r/m
                    # 0xE8 = CALL rel32
                    # 0x85 = TEST reg, r/m
                    # 0x0F = two-byte opcode
                    # 0xC7 = MOV r/m, imm
                    # 0xCC = INT3
            else:
                print(f'  ERROR: file offset too large!')
            
            # Also show surrounding context
            ctx_start = max(data_start_rva + cumulative_data, file_offset - 32)
            f.seek(ctx_start)
            context = f.read(128)
            ctx_off = file_offset - ctx_start
            print(f'  Context [{hex(ctx_start)} + {ctx_off}]:')
            print(f'    {context[:96].hex()}')
            break
        
        cumulative_data += range_size
    
    if found_range_idx == -1:
        print(f'\nCrash addr {hex(crash_addr)} not found in memory ranges!')
        
        # Find which module contains this address
        f.seek(stream_rva)
        for i in range(num_streams):
            sd = f.read(12)
            stype, ssize, srva = struct.unpack_from('<III', sd, 0)
            if stype == 4:
                print(f'\nModuleListStream at {hex(srva)}, size={ssize}')
                f.seek(srva)
                num_mods = struct.unpack_from('<I', f.read(4))[0]
                print(f'  Number of modules: {num_mods}')
                for m in range(num_mods):
                    mod_data = f.read(108)
                    mod_base = struct.unpack_from('<Q', mod_data, 8)[0]
                    mod_size = struct.unpack_from('<Q', mod_data, 24)[0]
                    name_rva = struct.unpack_from('<I', mod_data, 36)[0]
                    cur = f.tell()
                    f.seek(name_rva)
                    name_bytes = b''
                    while True:
                        b = f.read(1)
                        if b == b'\0' or not b:
                            break
                        name_bytes += b
                    f.seek(cur)
                    name = name_bytes.decode('utf-8', errors='replace')
                    
                    if mod_base <= crash_addr < mod_base + mod_size:
                        offset = crash_addr - mod_base
                        print(f'  *** IN MODULE: {name} ***')
                        print(f'      base={hex(mod_base)}, size={hex(mod_size)}')
                        print(f'      MODULE OFFSET: {hex(offset)} ({offset})')
                break
