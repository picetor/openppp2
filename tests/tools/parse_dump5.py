import struct

dmp_path = r'C:\Users\35295\Desktop\openppp2\openppp2-windows-x64-debug_20260702_1622-20260703-003659.dmp'

with open(dmp_path, 'rb') as f:
    header = f.read(32)
    sig, ver, num_streams, stream_rva, _, _, flags = struct.unpack_from('<IIIIIIQ', header, 0)
    
    f.seek(stream_rva)
    streams = {}
    for i in range(num_streams):
        sd = f.read(12)
        stype, ssize, srva = struct.unpack_from('<III', sd, 0)
        streams[stype] = (srva, ssize)
    
    # Exception stream (type 6)
    exc_rva, exc_size = streams[6]
    f.seek(exc_rva)
    exc_data = f.read(exc_size)
    crash_tid = struct.unpack_from('<I', exc_data, 0)[0]
    
    # Thread list (type 3)
    thr_rva, thr_size = streams[3]
    f.seek(thr_rva)
    num_threads = struct.unpack_from('<I', f.read(4))[0]
    
    for t in range(num_threads):
        raw = f.read(56)  # MINIDUMP_THREAD is 56 bytes
        thr_tid = struct.unpack_from('<I', raw, 0)[0]  # ThreadId at offset 0
        
        if thr_tid == crash_tid:
            # MINIDUMP_MEMORY_DESCRIPTOR for Stack is at offset 24:
            # Stack.StartOfMemoryRange QWORD at 24, Stack.DataSize DWORD at 32, Stack.Rva DWORD at 36
            # MINIDUMP_MEMORY_DESCRIPTOR for Context is at offset 40:
            # Context.StartOfMemoryRange QWORD at 40, Context.DataSize DWORD at 48, Context.Rva DWORD at 52
            context_rva = struct.unpack_from('<I', raw, 52)[0]
            context_size = struct.unpack_from('<I', raw, 48)[0]
            context_start = struct.unpack_from('<Q', raw, 40)[0]
            stack_rva_val = struct.unpack_from('<I', raw, 36)[0]
            stack_size_val = struct.unpack_from('<I', raw, 32)[0]
            stack_start_val = struct.unpack_from('<Q', raw, 24)[0]
            
            print(f'Context RVA: {hex(context_rva)}, size: {context_size}, start: {hex(context_start)}')
            print(f'Stack RVA: {hex(stack_rva_val)}, size: {stack_size_val}, start: {hex(stack_start_val)}')
            print(f'Context RVA: {hex(context_rva)}')
            
            f.seek(context_rva)
            # x64 CONTEXT is 0x4D0 (1232) bytes
            ctx = f.read(0x4D0)
            
            if len(ctx) >= 0x4D0:
                # CONTEXT_FLAGS at offset 0
                ctx_flags = struct.unpack_from('<I', ctx, 0)[0]
                print(f'Context flags: {ctx_flags:#x}')
                
                # Parse register values from CONTEXT
                # See: https://www.nirsoft.net/kernel_struct/vista/CONTEXT.html
                # For x64:
                # 0x000: ContextFlags (DWORD)
                # 0x008: P1Home (QWORD)
                # 0x010: P2Home (QWORD)
                # 0x018: P3Home (QWORD)
                # 0x020: P4Home (QWORD)
                # 0x028: P5Home (QWORD)
                # 0x030: P6Home (QWORD)
                # 0x038: ContextFlags2 (DWORD)
                # 0x03C: Pad (DWORD)
                # 0x040: SegCS (QWORD)
                # 0x048: SegDS (QWORD)
                # 0x050: SegES (QWORD)
                # 0x058: SegFS (QWORD)
                # 0x060: SegGS (QWORD)
                # 0x068: SegSS (QWORD)
                # 0x070: EFlags (DWORD)
                # 0x078: Dr0 (QWORD)
                # 0x080: Dr1 (QWORD)
                # 0x088: Dr2 (QWORD)
                # 0x090: Dr3 (QWORD)
                # 0x098: Dr6 (QWORD)
                # 0x0A0: Dr7 (QWORD)
                # 0x0A8: Rax (QWORD)
                # 0x0B0: Rcx (QWORD)
                # 0x0B8: Rdx (QWORD)
                # 0x0C0: Rbx (QWORD)
                # 0x0C8: Rsp (QWORD)
                # 0x0D0: Rbp (QWORD)
                # 0x0D8: Rsi (QWORD)
                # 0x0E0: Rdi (QWORD)
                # 0x0E8: R8 (QWORD)
                # 0x0F0: R9 (QWORD)
                # 0x0F8: R10 (QWORD)
                # 0x100: R11 (QWORD)
                # 0x108: R12 (QWORD)
                # 0x110: R13 (QWORD)
                # 0x118: R14 (QWORD)
                # 0x120: R15 (QWORD)
                # 0x128: Rip (QWORD)

                # Actually, for MS x64 CONTEXT, the layout is different from what I listed
                # Let me try both common offsets:
                
                # CONTEXT64 format used by Windows:
                # Offsets (from winnt.h/_CONTEXT):
                # 0x000: ULONG64 P1Home;
                # 0x008: ULONG64 P2Home;
                # 0x010: ULONG64 P3Home;
                # 0x018: ULONG64 P4Home;
                # 0x020: ULONG64 P5Home;
                # 0x028: ULONG64 P6Home;
                # 0x030: ULONG32 ContextFlags;
                # 0x034: ULONG32 MxCsr;
                # 0x038: ULONG16 SegCs;
                # 0x03A: ULONG16 SegDs;
                # 0x03C: ULONG16 SegEs;
                # 0x03E: ULONG16 SegFs;
                # 0x040: ULONG16 SegGs;
                # 0x042: ULONG16 SegSs;
                # 0x044: ULONG32 EFlags;
                # 0x048: ULONG64 Dr0;
                # 0x050: ULONG64 Dr1;
                # 0x058: ULONG64 Dr2;
                # 0x060: ULONG64 Dr3;
                # 0x068: ULONG64 Dr6;
                # 0x070: ULONG64 Dr7;
                # 0x078: ULONG64 Rax;
                # 0x080: ULONG64 Rcx;
                # 0x088: ULONG64 Rdx;
                # 0x090: ULONG64 Rbx;
                # 0x098: ULONG64 Rsp;
                # 0x0A0: ULONG64 Rbp;
                # 0x0A8: ULONG64 Rsi;
                # 0x0B0: ULONG64 Rdi;
                # 0x0B8: ULONG64 R8;
                # 0x0C0: ULONG64 R9;
                # 0x0C8: ULONG64 R10;
                # 0x0D0: ULONG64 R11;
                # 0x0D8: ULONG64 R12;
                # 0x0E0: ULONG64 R13;
                # 0x0E8: ULONG64 R14;
                # 0x0F0: ULONG64 R15;
                # 0x0F8: ULONG64 Rip;
                
                reg_offsets = {
                    'RAX': 0x078, 'RCX': 0x080, 'RDX': 0x088, 'RBX': 0x090,
                    'RSP': 0x098, 'RBP': 0x0A0, 'RSI': 0x0A8, 'RDI': 0x0B0,
                    'R8': 0x0B8, 'R9': 0x0C0, 'R10': 0x0C8, 'R11': 0x0D0,
                    'R12': 0x0D8, 'R13': 0x0E0, 'R14': 0x0E8, 'R15': 0x0F0,
                    'RIP': 0x0F8,
                }
                
                print(f'\n=== REGISTERS AT CRASH ===')
                for name, off in reg_offsets.items():
                    val = struct.unpack_from('<Q', ctx, off)[0]
                    marker = ''
                    if name == 'RAX':
                        marker = f' (OFFSET: 0x{val:X})'
                        if val == 0x65C:
                            marker += ' = &state_ within WintunAdapter'
                    elif name == 'RCX':
                        marker = ' = this pointer (if calling member function)'
                        if val == 0:
                            marker += ' *** THIS IS NULL! ***'
                    elif name == 'RIP':
                        marker = ' = crash instruction'
                    elif name == 'RSP':
                        marker = ' = stack pointer'
                    elif name == 'RBP':
                        marker = ' = frame pointer'
                    print(f'  {name}: 0x{val:016x}{marker}')
                
                # Check if RCX is related to the target address
                rcx = struct.unpack_from('<Q', ctx, 0x080)[0]
                rax = struct.unpack_from('<Q', ctx, 0x078)[0]
                print(f'\n=== ANALYSIS ===')
                print(f'Crash: mov ecx, dword ptr [rax] at RIP=0x{struct.unpack_from("<Q", ctx, 0x0F8)[0]:x}')
                print(f'RAX = {hex(rax)} (address being read from)')
                print(f'RCX = {hex(rcx)} (this pointer)')
                
                if rax == 0x65C:
                    print(f'\nRAX=0x65C means reading from low address.')
                    print(f'If RCX=0 (NULL this), then a member at offset 0x65C from NULL is being accessed.')
                    if rcx == 0:
                        print(f'\n*** CONCLUSION: THIS=NULL, accessing member at offset 0x65C! ***')
                    else:
                        offset_from_rcx = rax - rcx
                        print(f'\nMember at offset 0x{offset_from_rcx:X} from RCX')
                
                # Also try the stack content
                rsp = struct.unpack_from('<Q', ctx, 0x098)[0]
                print(f'\nRSP = {hex(rsp)}')
                print(f'Frame base (RBP) = {hex(struct.unpack_from("<Q", ctx, 0x0A0)[0])}')
                
                # Read the actual RIP bytes from the exe file (we know the file layout)
                exe_path = r'C:\Users\35295\Desktop\openppp2\openppp2-windows-x64-debug_20260702_1622.exe'
                import os
                if os.path.exists(exe_path):
                    with open(exe_path, 'rb') as exe:
                        # .text section: VA=0x1000, FileOffset=0x400, VSize=0xdd9a38
                        # RIP at 0x7ff7fc2bea29, image base 0x7ff7fb910000
                        # RVA = 0x9AEA29
                        rva = 0x9AEA29
                        section_va = 0x1000
                        section_fo = 0x400
                        # The code is always at the same offset in the file regardless of ASLR
                        # section_fo + (rva - section_va) = 0x400 + (0x9AEA29 - 0x1000) = 0x9ADE29
                        file_offset = section_fo + (rva - section_va)
                        exe.seek(file_offset)
                        code_bytes = exe.read(32)
                        print(f'\nCode at RIP (from EXE file):')
                        from capstone import *
                        md = Cs(CS_ARCH_X86, CS_MODE_64)
                        for insn in md.disasm(code_bytes, 0x7ff7fc2bea29):
                            print(f'  0x{insn.address:x}: {insn.mnemonic:8s} {insn.op_str}')
            break
