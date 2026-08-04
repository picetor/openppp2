#!/usr/bin/env python3
"""Parse a Windows minidump to extract crash info: exception record, crash thread
registers (CONTEXT), thread stack, module base, and a rough call stack."""
import struct, sys

def parse(path):
    with open(path, 'rb') as f:
        data = f.read()

    # --- MINIDUMP_HEADER ---
    assert data[0:4] == b'MDMP', 'not a minidump'
    nstreams = struct.unpack_from('<I', data, 8)[0]
    streams = {}
    for i in range(nstreams):
        stype, ssize, srv = struct.unpack_from('<III', data, 20 + i * 12)
        streams[stype] = (srv, ssize)

    # --- Exception stream (type 6) ---
    er = streams.get(6, (None,))[0]
    tid = struct.unpack_from('<I', data, er)[0]
    ec = struct.unpack_from('<I', data, er + 8)[0]
    ea = struct.unpack_from('<Q', data, er + 24)[0]
    print(f'EXCEPTION: ThreadId=0x{tid:X} Code=0x{ec:08X} Addr=0x{ea:016X}')

    # --- Thread list (type 3) ---
    tr = streams[3][0]
    nt = struct.unpack_from('<I', data, tr)[0]
    print(f'THREADS: {nt}')
    crash_ctx_rva = None
    crash_stack = None
    for i in range(nt):
        # MINIDUMP_THREAD is 48 bytes:
        # ThreadId@0(4) SuspendCount@4(4) PriorityClass@8(4) Priority@12(4)
        # Teb@16(8) Stack{StartOfMemoryRange@24(8), Size@32(8)}
        # ThreadContext{DataSize@40(4), Rva@44(4)}
        t = tr + 4 + i * 48
        t_tid = struct.unpack_from('<I', data, t)[0]
        if t_tid == tid:
            stack_start = struct.unpack_from('<Q', data, t + 24)[0]
            stack_size = struct.unpack_from('<Q', data, t + 32)[0]
            ctx_size = struct.unpack_from('<I', data, t + 40)[0]
            ctx_rva = struct.unpack_from('<I', data, t + 44)[0]
            print(f'CRASH THREAD tid=0x{t_tid:X} stack=0x{stack_start:X}+0x{stack_size:X} ctx_size=0x{ctx_size:X} ctx_rva=0x{ctx_rva:X}')
            crash_ctx_rva = ctx_rva
            crash_stack = (stack_start, stack_size)

    # --- Crash thread CONTEXT (AMD64: 0x4D0 bytes) ---
    # Locate by searching for the RIP bytes across memory regions, or use
    # known values from the exception record. Fall back to the exception
    # address, then walk the stack using RSP if a region matches.
    if crash_ctx_rva:
        c = crash_ctx_rva
        def g(off):
            return struct.unpack_from('<Q', data, c + off)[0]
        rbx, rcx, rdx = g(0x90), g(0x80), g(0x88)
        rsi, rdi, rbp = g(0xA8), g(0xB0), g(0xA0)
        rsp = g(0x98)
        rip = g(0xF8)
        print(f'RBX=0x{rbx:016X} RCX=0x{rcx:016X} RDX=0x{rdx:016X}')
        print(f'RSI=0x{rsi:016X} RDI=0x{rdi:016X} RBP=0x{rbp:016X}')
        print(f'RSP=0x{rsp:016X} RIP=0x{rip:016X}')
    else:
        # Fallback: search for CONTEXT by ContextFlags=0x10005F
        import re as _re
        pat = _re.compile(b'\x5f\x00\x10\x00')  # 0x10005F little-endian
        found_ctx = None
        # scan candidate regions from memory list
        memr_tmp = streams[5][0]
        nrt = struct.unpack_from('<I', data, memr_tmp)[0]
        doff_tmp = memr_tmp + 4 + nrt * 16
        for i in range(nrt):
            start, size = struct.unpack_from('<QQ', data, memr_tmp + 4 + i * 16)
            if size > 0x2000:
                blob = data[doff_tmp:doff_tmp + size]
                m = pat.search(blob)
                if m:
                    # CONTEXT layout: ContextFlags@0x30
                    off = m.start() - 0x30
                    if off >= 0:
                        cf = struct.unpack_from('<I', data, doff_tmp + off + 0x30)[0]
                        if cf == 0x10005F:
                            found_ctx = doff_tmp + off
                            break
            doff_tmp += size
        if found_ctx:
            def g(off):
                return struct.unpack_from('<Q', data, found_ctx + off)[0]
            rbx, rcx, rdx = g(0x90), g(0x80), g(0x88)
            rsi, rdi, rbp = g(0xA8), g(0xB0), g(0xA0)
            rsp = g(0x98)
            rip = g(0xF8)
            print(f'[found CONTEXT @0x{found_ctx:X}]')
            print(f'RBX=0x{rbx:016X} RCX=0x{rcx:016X} RDX=0x{rdx:016X}')
            print(f'RSI=0x{rsi:016X} RDI=0x{rdi:016X} RBP=0x{rbp:016X}')
            print(f'RSP=0x{rsp:016X} RIP=0x{rip:016X}')
        else:
            print('CONTEXT NOT FOUND; using exception address only')
            rsp = rip = rbx = rcx = rsi = rbp = 0

    # --- Module list (type 4): entries are 108 bytes ---
    mr = streams[4][0]
    nmod = struct.unpack_from('<I', data, mr)[0]
    bases = []
    for m in range(nmod):
        moff = mr + 4 + m * 108
        mbase = struct.unpack_from('<Q', data, moff)[0]
        bases.append(mbase)
        if m < 2 or m == nmod - 1:
            print(f'  module[{m}] base=0x{mbase:016X}')

    # --- Memory list (type 5): descs are {start Q @0, size I @8, pad I @12}
    # (16B stride); region data follows after all descs ---
    memr = streams[5][0]
    nr = struct.unpack_from('<I', data, memr)[0]
    desc_off = memr + 4
    data_off = desc_off + nr * 16
    regions = []
    for i in range(nr):
        start = struct.unpack_from('<Q', data, desc_off + i * 16)[0]
        size = struct.unpack_from('<I', data, desc_off + i * 16 + 8)[0]
        regions.append((start, size, data_off))
        data_off += size
    print(f'MEMORY regions: {nr}, data starts at 0x{desc_off + nr * 16:X}')

    def read_va(va, n):
        for start, size, doff in regions:
            if start <= va and va + n <= start + size:
                o = doff + (va - start)
                return data[o:o + n]
        return None

    # --- Stack data ---
    sdata = None
    sstart = 0
    if rsp:
        for start, size, doff in regions:
            if start <= rsp < start + size:
                sstart = rsp & ~0xF
                take = min(0x4000, size - (sstart - start))
                if take > 0:
                    sdata = data[doff + (sstart - start):doff + (sstart - start) + take]
                    print(f'STACK READ OK: 0x{sstart:X}+0x{take:X} (region 0x{start:X}+0x{size:X})')
                break
        if not sdata:
            print('STACK DATA NOT FOUND in memory list')

    # Build module address ranges for classification
    mod_ranges = []
    for m in range(nmod):
        moff = mr + 4 + m * 108
        mbase = struct.unpack_from('<Q', data, moff)[0]
        msize = struct.unpack_from('<Q', data, moff + 8)[0]
        if msize < 0x100000000:  # sane size
            mod_ranges.append((mbase, mbase + msize))

    def classify(addr):
        for mbase, mend in mod_ranges:
            if mbase <= addr < mend:
                rva = addr - mbase
                if mbase == bases[0]:
                    return f'ppp+0x{rva:X}'
                return f'[mod+0x{rva:X}]'
        return f'0x{addr:X}'

    # unwind: walk stack qwords, collect addresses that look like code
    print('\nSTACK WALK (qwords in ppp.exe range, from RSP upward):')
    if sdata:
        limit = min(0x4000, len(sdata))
        n = limit // 8
        found = 0
        for i in range(n):
            q = struct.unpack_from('<Q', sdata, i * 8)[0]
            if bases and bases[0] <= q < bases[0] + 0x2000000:
                print(f'  RSP+0x{i*8:04X}: {classify(q)}')
                found += 1
                if found > 80:
                    break

if __name__ == '__main__':
    for p in sys.argv[1:]:
        print('=' * 70)
        print('DUMP:', p)
        parse(p)
