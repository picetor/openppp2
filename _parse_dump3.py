# Parse minidump with reversed memory descriptor fields to find the crash thread's stack
# and the correct memory ranges
import struct
import os

dmp_path = r'C:\Users\35295\Desktop\openppp2\openppp2-windows-x64-debug_20260702_1622-20260703-003659.dmp'
crash_addr = 0x7FF7FC2BEA29

with open(dmp_path, 'rb') as f:
    header = f.read(32)
    sig, ver, num_streams, stream_rva, _, _, flags = struct.unpack_from('<IIIIIIQ', header, 0)
    print(f'NumStreams: {num_streams}')
    
    f.seek(stream_rva)
    streams = {}
    for i in range(num_streams):
        sd = f.read(12)
        stype, ssize, srva = struct.unpack_from('<III', sd, 0)
        streams[stype] = (srva, ssize)
        print(f'  Stream {stype}: RVA={hex(srva)}, size={ssize}')
    
    # Find exception stream (type 6)
    if 6 in streams:
        exc_rva, exc_size = streams[6]
        f.seek(exc_rva)
        exc_data = f.read(exc_size)
        
        # MINIDUMP_EXCEPTION_STREAM
        tid = struct.unpack_from('<I', exc_data, 0)[0]
        print(f'\nException thread ID: {tid} (0x{tid:x})')
        
        # MINIDUMP_EXCEPTION at offset 8
        exc_code = struct.unpack_from('<I', exc_data, 8)[0]
        exc_flags = struct.unpack_from('<I', exc_data, 12)[0]
        exc_addr = struct.unpack_from('<Q', exc_data, 24)[0]
        num_params = struct.unpack_from('<I', exc_data, 20)[0]
        param1 = struct.unpack_from('<Q', exc_data, 32)[0]
        param2 = struct.unpack_from('<Q', exc_data, 40)[0]
        
        print(f'Exception code: {hex(exc_code)} (ACCESS_VIOLATION={exc_code==0xC0000005})')
        print(f'Exception address: {hex(exc_addr)}')
        print(f'Num params: {num_params}')
        
        if exc_code == 0xC0000005:
            op = 'WRITE' if param1 & 1 else 'READ'
            print(f'  Operation: {op}')
            print(f'  Target address: {hex(param2)}')
    
    # Find thread list stream (type 3)
    if 3 in streams:
        thr_rva, thr_size = streams[3]
        f.seek(thr_rva)
        num_threads = struct.unpack_from('<I', f.read(4))[0]
        print(f'\nThreads: {num_threads}')
        
        # Each MINIDUMP_THREAD is 48 bytes
        for t in range(num_threads):
            thr = f.read(48)
            thr_tid = struct.unpack_from('<I', thr, 4)[0]
            if thr_tid == tid:  # Crash thread
                stack_rva = struct.unpack_from('<I', thr, 12)[0]
                stack_size = struct.unpack_from('<I', thr, 16)[0]
                # MINIDUMP_MEMORY_DESCRIPTOR at offset 8: StartOfMemoryRange (QWORD) + DataSize (DWORD) + RVA (DWORD)
                stack_start = struct.unpack_from('<Q', thr, 8)[0]
                stack_data_rva = struct.unpack_from('<I', thr, 24)[0]  # RVA
                stack_data_size = struct.unpack_from('<I', thr, 20)[0]  # DataSize from the DWORD
                
                print(f'\nCrash thread (TID={tid}):')
                print(f'  Stack start: {hex(stack_start)}')
                print(f'  Stack bytes (RVA): {hex(stack_data_rva)}, size: {stack_data_size}')
                
                # Read stack contents
                if stack_data_size > 0 and stack_data_size < 100000:
                    f.seek(stack_data_rva)
                    stack_bytes = f.read(stack_data_size)
                    
                    # Walk the stack - look for return addresses
                    # x64: return addresses are 8-byte values on the stack
                    print(f'\n  Stack memory (first 256 bytes as QWORDs):')
                    for i in range(0, min(stack_data_size, 256), 8):
                        if i + 8 <= len(stack_bytes):
                            val = struct.unpack_from('<Q', stack_bytes, i)[0]
                            if val != 0:
                                # Check if value looks like a code address (in ppp.exe range)
                                in_ppp = 0x7FF7FB910000 <= val < 0x7FF7FB910000 + 0x2000000
                                in_ntdll = 0x7FFE00000000 <= val < 0x800000000000
                                marker = ''
                                if in_ppp:
                                    offset = val - 0x7FF7FB910000
                                    marker = f' <-- ppp.exe+0x{offset:X}'
                                elif in_ntdll:
                                    marker = ' <-- ntdll.dll range'
                                
                                print(f'    [{i:3d}] stack+{hex(i)}: {hex(val)}{marker}')
                break
    
    # Find Memory64ListStream (type 9) - read with REVERSED fields
    if 9 in streams:
        mem64_rva, mem64_size = streams[9]
        print(f'\n\nMemory64ListStream at {hex(mem64_rva)}')
        f.seek(mem64_rva)
        num_ranges = struct.unpack_from('<Q', f.read(8))[0]
        print(f'Num ranges: {num_ranges}')
        
        # Read descriptors with reversed interpretation (DataSize = first QWORD, Start = second QWORD)
        descriptors_rva = mem64_rva + 8
        data_start_rva = descriptors_rva + num_ranges * 16
        
        f.seek(descriptors_rva)
        ranges = []
        for i in range(num_ranges):
            raw = f.read(16)
            # Standard: Start, Size. Reversed (observed): DataSize, StartOfMemoryRange
            # Try standard first
            start1, size1 = struct.unpack_from('<QQ', raw, 0)
            # Try reversed
            size2, start2 = struct.unpack_from('<QQ', raw, 0)
            
            # Which interpretation gives sane sizes?
            if size1 < 0x1000000:  # < 16MB - sane for a page
                ranges.append((start1, size1, i))
            elif size2 < 0x1000000:
                ranges.append((start2, size2, i))
        
        # Find range containing cras address
        print(f'\nSearching crash addr {hex(crash_addr)} in {len(ranges)} sane ranges...')
        for start, size, idx in ranges:
            if start <= crash_addr < start + size:
                print(f'  FOUND range {idx}: {hex(start)}-{hex(start+size)}')
                # Read code bytes
                cum = 0
                f.seek(descriptors_rva)
                for j in range(idx):
                    raw = f.read(16)
                    s1, sz1 = struct.unpack_from('<QQ', raw, 0)
                    sz2, s2 = struct.unpack_from('<QQ', raw, 0)
                    if sz1 < 0x1000000:
                        cum += sz1
                    elif sz2 < 0x1000000:
                        cum += sz2
                
                offset = crash_addr - start
                file_off = data_start_rva + cum + offset
                file_size = os.path.getsize(dmp_path)
                print(f'  File offset: {hex(file_off)} (file size: {hex(file_size)})')
                
                if file_off + 64 <= file_size:
                    f.seek(file_off)
                    code = f.read(64)
                    print(f'  Code: {code.hex()[:64]}')
                break
        else:
            print(f'  Crash address NOT found in memory ranges')
            print(f'  (Executable code pages are typically excluded from minidumps)')
    
    # Find MemoryListStream (type 5) - the 32-bit version
    if 5 in streams:
        mem_rva, mem_size = streams[5]
        print(f'\n\nMemoryListStream (type 5) at {hex(mem_rva)}, size={mem_size}')
        f.seek(mem_rva)
        num_ranges32 = struct.unpack_from('<I', f.read(4))[0]
        print(f'  Num ranges: {num_ranges32}')
        
        # MINIDUMP_MEMORY_DESCRIPTOR: StartOfMemoryRange (8) + DataSize (4) + RVA (4) = 16 bytes
        f.seek(mem_rva + 4)
        for i in range(min(num_ranges32, 10)):
            raw = f.read(16)
            start, dsize, mrva = struct.unpack_from('<QII', raw, 0)
            if start <= crash_addr < start + dsize:
                print(f'  FOUND range {i}: start={hex(start)} size={hex(dsize)} rva={hex(mrva)}')
                offset = crash_addr - start
                f.seek(mrva + offset)
                code = f.read(64)
                print(f'  Code at crash: {code.hex()}')
                break
