from capstone import *

# Code bytes from crash at file offset 0x9ade29 in ppp.exe (RVA 0x9AEA29)
code_hex = "8b088bc18bd00b95080100004c8b85d80000004c8b85d8000000f0410fb1108bc875df8bc18945048b4504488da5e80000005f5dc3cccc4489442418895424104889442408534883ec20"

md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True

print("=== DISASSEMBLY OF CRASH SITE ===")
print(f"Crash at RVA 0x9AEA29 (offset 0x9AEA29 from image base 0x140000000)")
print()

code = bytes.fromhex(code_hex)
for i in md.disasm(code, 0x7FF7FC2BEA29):
    marker = ""
    if i.address == 0x7FF7FC2BEA29:
        marker = " <<< CRASH ADDRESS"
    print(f"  0x{i.address:x}: {i.mnemonic:8s} {i.op_str}{marker}")

print()
print("=== ANALYSIS ===")
print("The crash is at the FIRST instruction: `mov ecx, dword ptr [rax]`")
print("This reads memory at address stored in RAX (first argument in x64 calling convention)")
print("If RAX == NULL (0x0), this causes ACCESS_VIOLATION reading from NULL")

print()
print("=== FULL HEX ===")
for i in range(0, len(code), 16):
    hex_str = ' '.join(f'{b:02x}' for b in code[i:i+16])
    print(f"  {hex_str}")
