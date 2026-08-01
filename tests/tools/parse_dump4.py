import struct
import os

dmp_path = r'C:\Users\35295\Desktop\openppp2\openppp2-windows-x64-debug_20260702_1622-20260703-003659.dmp'
crash_addr = 0x7FF7FC2BEA29

with open(dmp_path, 'rb') as f:
    header = f.read(32)
    sig, ver, num_streams, stream_rva, _, _, flags = struct.unpack_from('<IIIIIIQ', header, 0)
    
    f.seek(stream_rva)
    streams = {}
    for i in range(num_streams):
        sd = f.read(12)
        stype, ssize, srva = struct.unpack_from('<III', sd, 0)
        streams[stype] = (srva, ssize)
    
    # Exception stream (type 6) - fix offsets
    if 6 in streams:
        exc_rva, exc_size = streams[6]
        f.seek(exc_rva)
        exc_data = f.read(exc_size)
        
        tid = struct.unpack_from('<I', exc_data, 0)[0]  # ThreadId
        # MINIDUMP_EXCEPTION starts at offset 8 (ThreadId + Alignment)
        exc_code = struct.unpack_from('<I', exc_data, 8)[0]
        exc_flags = struct.unpack_from('<I', exc_data, 12)[0]
        exc_record = struct.unpack_from('<Q', exc_data, 16)[0]
        exc_addr = struct.unpack_from('<Q', exc_data, 24)[0]
        num_params = struct.unpack_from('<I', exc_data, 32)[0]  # Fixed: was 20
        # param1 at offset 40 (32 + 8 for the DWORDs at 32/36)
        param1 = struct.unpack_from('<Q', exc_data, 40)[0] if num_params >= 1 else 0
        param2 = struct.unpack_from('<Q', exc_data, 48)[0] if num_params >= 2 else 0
        
        print(f'Exception thread: {tid} (0x{tid:x})')
        print(f'Exception code: {hex(exc_code)}')
        print(f'Exception flags: {hex(exc_flags)}')
        print(f'Exception address: {hex(exc_addr)}')
        print(f'Number parameters: {num_params}')
        if num_params >= 2:
            op = 'WRITE' if param1 & 1 else 'READ'
            print(f'  Operation: {op}')
            print(f'  Target address: {hex(param2)}')
    
    # Thread list (type 3)
    if 3 in streams:
        thr_rva, thr_size = streams[3]
        f.seek(thr_rva)
        num_threads = struct.unpack_from('<I', f.read(4))[0]
        print(f'\nTotal threads: {num_threads}')
        
        crash_thread_id = tid
        for t in range(num_threads):
            raw = f.read(48)
            thr_tid = struct.unpack_from('<I', raw, 4)[0]
            
            if thr_tid == crash_thread_id:
                # Read MINIDUMP_MEMORY_DESCRIPTOR for stack
                stack_start = struct.unpack_from('<Q', raw, 8)[0]  # StartOfMemoryRange (8 bytes at offset 8)
                stack_data_size = struct.unpack_from('<I', raw, 20)[0]  # DataSize (4 bytes at offset 20)
                stack_rva = struct.unpack_from('<I', raw, 24)[0]  # RVA (4 bytes at offset 24)
                
                # Also get context
                context_rva = struct.unpack_from('<I', raw, 40)[0]  # Context RVA
                
                print(f'\nCrash thread (TID={thr_tid}):')
                print(f'  Stack start: {hex(stack_start)}')
                print(f'  Stack data size: {stack_data_size} bytes')
                print(f'  Stack RVA: {hex(stack_rva)}')
                print(f'  Context RVA: {hex(context_rva)}')
                
                # Read the stack data
                f.seek(stack_rva)
                stack_data = f.read(stack_data_size)
                
                # Stack grows DOWNWARD in x64. 
                # The stack pointer (RSP) at crash time points to top of stack
                # Return addresses are at RSP (top) going upward
                print(f'\n  Stack contents (QWORDs, from top of captured region):')
                
                # Find likely return addresses in the stack
                ppp_base = 0x7FF7FB910000
                ppp_size = 0x2000000
                
                for i in range(0, min(stack_data_size, 1024), 8):
                    if i + 8 <= len(stack_data):
                        val = struct.unpack_from('<Q', stack_data, i)[0]
                        if val != 0:
                            in_ppp = ppp_base <= val < ppp_base + ppp_size
                            in_dll = 0x7FF000000000 <= val < 0x800000000000
                            
                            if in_ppp or in_dll:
                                offset_in_mod = val - ppp_base if in_ppp else 0
                                marker = ''
                                if in_ppp:
                                    # Skip if this is in the code section (.text at RVA 0x1000)
                                    marker = f' ppp.exe+0x{offset_in_mod:X}'
                                elif in_dll:
                                    marker = f' DLL range'
                                print(f'    stack[{i:3d}]: {hex(val)}{marker}')
                
                # Try to read the Context record
                if context_rva > 0:
                    f.seek(context_rva)
                    ctx = f.read(0x4D0)  # ARM64EC_CONTEXT is 1232 bytes, x64 CONTEXT is 1232 (0x4D0)
                    if len(ctx) >= 0x4D0:
                        ctx_flags = struct.unpack_from('<I', ctx, 0)[0]
                        print(f'\n  Context flags: {hex(ctx_flags)}')
                        
                        # Integer registers in CONTEXT (x64):
                        # P1Home-R9: offsets 8, 16, 24, 32, 40, 48, 56, 64
                        # R10-R15: offsets 144, 152, 160, 168, 176, 184
                        # RDI: offset 72
                        # RSI: offset 80
                        # RBX: offset 88
                        # RDX: offset 96
                        # RCX: offset 104
                        # RAX: offset 112
                        # RBP: offset 200
                        # RSP: offset 216
                        # RIP: offset 248
                        
                        regs = {
                            'P1Home': 8, 'P2Home': 16, 'P3Home': 24, 'P4Home': 32,
                            'P5Home': 40, 'P6Home': 48,
                            'RDI': 72, 'RSI': 80, 'RBX': 88, 'RDX': 96, 
                            'RCX': 104, 'RAX': 112,
                            'R8': 120, 'R9': 128, 'R10': 136, 'R11': 144,
                            'R12': 152, 'R13': 160, 'R14': 168, 'R15': 176,
                            'RBP': 200, 'RSP': 216, 'RIP': 248
                        }
                        
                        print(f'\n  Registers at crash:')
                        for name, off in regs.items():
                            val = struct.unpack_from('<Q', ctx, off)[0]
                            marker = ''
                            if name == 'RAX' and val == 0:
                                marker = ' <<< NULL POINTER!'
                            elif name == 'RIP':
                                marker = ' <<< CRASH ADDRESS'
                            print(f'    {name}: {hex(val)}{marker}')
                        
                        print(f'\n  RAW RIP code bytes verification:')
                        # Read code at RIP from the CONTEXT'S memory range
                        # Can't get it here since we don't have the memory ranges
                        # But we already know the code from the EXE file
                break
