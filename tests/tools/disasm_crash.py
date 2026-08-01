# Disassemble the crash site code bytes
# Code from file offset 0x9ade29 in ppp.exe (RVA 0x9AEA29)
code_hex = "8b088bc18bd00b95080100004c8b85d80000004c8b85d8000000f0410fb1108bc875df8bc18945048b4504488da5e80000005f5dc3cccc4489442418895424104889442408534883ec20"

# Let's decode this byte by byte as x86_64
import subprocess

try:
    # Try capstone if available
    from capstone import *
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    for i in md.disasm(bytes.fromhex(code_hex), 0x7FF7FC2BEA29):
        print(f"  0x{i.address:x}: {i.mnemonic} {i.op_str}")
except ImportError:
    print("capstone not available, manual decoding:")

# Manual x86_64 decoding
code = bytes.fromhex(code_hex)
pos = 0
addr = 0x7FF7FC2BEA29

while pos < len(code) and pos < 64:
    b = code[pos]
    if pos == 0:
        print(f"\n>>> CRASH AT 0x{addr:x} <<<")
    
    if b == 0x8b and pos + 2 <= len(code):
        modrm = code[pos+1]
        mod = (modrm >> 6) & 3
        reg = (modrm >> 3) & 7
        rm = modrm & 7
        # Check for REX prefix before this byte
        rex = 0
        if pos > 0 and (code[pos-1] & 0xf0) == 0x40:
            rex = code[pos-1]
            rreg = reg | (1 if rex & 4 else 0)
            rrm = rm | (1 if rex & 1 else 0)
        else:
            rreg = reg
            rrm = rm
        
        reg_names = ['eax','ecx','edx','ebx','esp','ebp','esi','edi',
                     'r8d','r9d','r10d','r11d','r12d','r13d','r14d','r15d']
        rm_names = ['[eax]','[ecx]','[edx]','[ebx]','[esp]','[ebp]','[esi]','[edi]',
                    '[r8]','[r9]','[r10]','[r11]','[r12]','[r13]','[r14]','[r15]']
        
        if mod == 0 and rm == 5:
            disp = struct.unpack_from('<i', code, pos+2)[0]
            op_str = f"{reg_names[rreg]}, [{disp:#x}]"
            pos += 6
        elif mod == 0:
            op_str = f"{reg_names[rreg]}, {rm_names[rrm]}"
            pos += 2
        elif mod == 1:
            disp = code[pos+2]
            if disp >= 128: disp -= 256
            op_str = f"{reg_names[rreg]}, {rm_names[rrm]}+{disp:#x}" if disp >= 0 else f"{reg_names[rreg]}, {rm_names[rrm]}{disp:#x}"
            pos += 3
        elif mod == 2:
            disp = struct.unpack_from('<i', code, pos+2)[0]
            op_str = f"{reg_names[rreg]}, {rm_names[rrm]}+{disp:#x}" if disp >= 0 else f"{reg_names[rreg]}, {rm_names[rrm]}{disp:#x}"
            pos += 6
        elif mod == 3:
            op_str = f"{reg_names[rreg]}, {reg_names[rrm]}"
            pos += 2
        
        marker = " <<< CRASH" if pos == 2 else ""
        print(f"  0x{addr:x}: 8b {code[pos-1]:02x}  mov {op_str}{marker}")
        addr += 2
    elif b == 0x0b and pos + 2 <= len(code):  # OR r32, r/m32
        modrm = code[pos+1]
        mod = (modrm >> 6) & 3
        reg = (modrm >> 3) & 7
        rm = modrm & 7
        disp = struct.unpack_from('<i', code, pos+2)[0]
        reg_names = ['eax','ecx','edx','ebx','esp','ebp','esi','edi',
                     'r8d','r9d','r10d','r11d','r12d','r13d','r14d','r15d']
        op_str = f"{reg_names[reg]}, [rbp+{disp:#x}]"
        print(f"  0x{addr:x}: 0b {code[pos+1]:02x} {disp:08x} or {op_str}")
        addr += 6
        pos += 6
    elif b == 0xf0:  # LOCK prefix
        print(f"  0x{addr:x}: f0     lock prefix")
        addr += 1
        pos += 1
    elif b == 0x41:  # REX.B prefix
        print(f"  0x{addr:x}: 41     rex.b")
        addr += 1
        pos += 1
    elif b == 0x0f and pos + 3 <= len(code):
        op2 = code[pos+1]
        modrm = code[pos+2]
        mod = (modrm >> 6) & 3
        reg = (modrm >> 3) & 7
        rm = modrm & 7
        
        if op2 == 0xb1:  # CMPXCHG r/m32, r32
            reg_names = ['eax','ecx','edx','ebx','esp','ebp','esi','edi',
                         'r8d','r9d','r10d','r11d','r12d','r13d','r14d','r15d']
            rm_names = ['[eax]','[ecx]','[edx]','[ebx]','[esp]','[ebp]','[esi]','[edi]',
                        '[r8]','[r9]','[r10]','[r11]','[r12]','[r13]','[r14]','[r15]']
            op_str = f"dword ptr {rm_names[rm]}, {reg_names[reg]}"
            print(f"  0x{addr:x}: 0f {op2:02x} {modrm:02x} cmpxchg {op_str}")
            addr += 3
            pos += 3
        
        # Continue after
        continue
    elif b == 0x8b and pos + 2 <= len(code):
        # Already handled above (first case)
        pass
    elif b == 0x89:  # MOV r/m32, r32
        modrm = code[pos+1]
        mod = (modrm >> 6) & 3
        reg = (modrm >> 3) & 7
        rm = modrm & 7
        reg_names = ['eax','ecx','edx','ebx','esp','ebp','esi','edi',
                     'r8d','r9d','r10d','r11d','r12d','r13d','r14d','r15d']
        if mod == 2:
            disp = struct.unpack_from('<i', code, pos+2)[0]
            print(f"  0x{addr:x}: 89 {modrm:02x} {disp:08x} mov [rbp+{disp:#x}], {reg_names[reg]}")
            addr += 6
            pos += 6
        elif mod == 0:
            print(f"  0x{addr:x}: 89 {modrm:02x} mov {rm_names[rm] if 'rm_names' in dir() else f'[{rm}]'}, {reg_names[reg]}")
            addr += 2
            pos += 2
        else:
            addr += 2
            pos += 2
    elif b == 0x48:  # REX.W prefix
        next_b = code[pos+1] if pos+1 < len(code) else 0
        if next_b == 0x8d:  # LEA
            modrm = code[pos+2]
            mod = (modrm >> 6) & 3
            reg = (modrm >> 3) & 7
            rm = modrm & 7
            reg64 = ['rax','rcx','rdx','rbx','rsp','rbp','rsi','rdi',
                     'r8','r9','r10','r11','r12','r13','r14','r15']
            if mod == 2:
                disp = struct.unpack_from('<i', code, pos+3)[0]
                print(f"  0x{addr:x}: 48 8d {modrm:02x} {disp:08x} lea {reg64[reg]}, [rbp+{disp:#x}]")
                addr += 7
                pos += 7
            else:
                addr += 1
                pos += 1
        elif next_b == 0x8b:  # MOV r64, r/m64
            modrm = code[pos+2]
            reg64 = ['rax','rcx','rdx','rbx','rsp','rbp','rsi','rdi',
                     'r8','r9','r10','r11','r12','r13','r14','r15']
            print(f"  0x{addr:x}: 48 8b {modrm:02x} mov ...")
            addr += 3
            pos += 3
        else:
            addr += 1
            pos += 1
    elif b == 0x4c:  # REX.WR prefix
        next_b = code[pos+1] if pos+1 < len(code) else 0
        if next_b == 0x8b:  # MOV r64, r/m64
            modrm = code[pos+2]
            mod = (modrm >> 6) & 3
            rm = modrm & 7
            reg = (modrm >> 3) & 7
            rm64 = ['[rax]','[rcx]','[rdx]','[rbx]','[rsp]','[rbp]','[rsi]','[rdi]',
                    '[r8]','[r9]','[r10]','[r11]','[r12]','[r13]','[r14]','[r15]']
            reg8 = ['r8','r9','r10','r11','r12','r13','r14','r15']
            reg4 = ['r8d','r9d','r10d','r11d','r12d','r13d','r14d','r15d']
            if mod == 2:
                disp = struct.unpack_from('<i', code, pos+3)[0]
                print(f"  0x{addr:x}: 4c 8b {modrm:02x} {disp:08x} mov {reg8[reg]}, [rbp+{disp:#x}]")
                addr += 7
                pos += 7
            else:
                addr += 3
                pos += 3
        elif next_b == 0x89:  # MOV r/m64, r64
            addr += 1
            pos += 1
        else:
            addr += 1
            pos += 1
    elif b == 0x5f:  # POP rdi
        print(f"  0x{addr:x}: 5f     pop rdi")
        addr += 1
        pos += 1
    elif b == 0x5d:  # POP rbp
        print(f"  0x{addr:x}: 5d     pop rbp")
        addr += 1
        pos += 1
    elif b == 0xc3:  # RET
        print(f"  0x{addr:x}: c3     ret")
        addr += 1
        pos += 1
    elif b == 0xcc:  # INT3
        print(f"  0x{addr:x}: cc     int3")
        addr += 1
        pos += 1
    elif b == 0x44:  # REX.R prefix
        next_b = code[pos+1] if pos+1 < len(code) else 0
        if next_b == 0x89:  # MOV r/m32, r32
            modrm = code[pos+2]
            mod = (modrm >> 6) & 3
            rm = modrm & 7
            if mod == 2:
                disp = struct.unpack_from('<i', code, pos+3)[0]
                print(f"  0x{addr:x}: 44 89 {modrm:02x} {disp:08x} mov [rbp+{disp:#x}], r8d")
                addr += 7
                pos += 7
            else:
                addr += 3
                pos += 3
        elif next_b == 0x8b:  # MOV
            addr += 1
            pos += 1
        else:
            addr += 1
            pos += 1
    elif b == 0x53:  # PUSH rbx
        print(f"  0x{addr:x}: 53     push rbx")
        addr += 1
        pos += 1
    elif b == 0x48:  # REX.W
        # Already handled partially above
        addr += 1
        pos += 1
    elif b == 0x83:  # ALU r/m, imm8
        addr += 1
        pos += 1
    elif b == 0x20:  # SUB r/m, r
        addr += 1
        pos += 1
    elif b == 0xec:  # IN AL, DX
        addr += 1
        pos += 1
    else:
        print(f"  0x{addr:x}: {b:02x}    ...")
        addr += 1
        pos += 1

print(f"\n\nFull code hex dump:")
for i in range(0, len(code), 16):
    hex_str = ' '.join(f'{b:02x}' for b in code[i:i+16])
    print(f"  {hex_str}")
