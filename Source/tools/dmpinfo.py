"""Prints a minidump's exception code/address (as an RVA of bfh_server_Final.exe when it is inside it), the
faulting thread's registers, and return-address candidates on its stack. Usage: python dmpinfo.py <file.dmp>"""
import struct, sys

data = open(sys.argv[1], "rb").read()
sig, ver, nstreams, dir_rva = struct.unpack_from("<4sIII", data, 0)
streams = {}
for i in range(nstreams):
    t, size, rva = struct.unpack_from("<III", data, dir_rva + i * 12)
    streams[t] = (size, rva)

mods = []
if 4 in streams:   # ModuleListStream
    _, rva = streams[4]
    n = struct.unpack_from("<I", data, rva)[0]
    for i in range(n):
        base, size = struct.unpack_from("<QI", data, rva + 4 + i * 108)
        name_rva = struct.unpack_from("<I", data, rva + 4 + i * 108 + 20)[0]
        ln = struct.unpack_from("<I", data, name_rva)[0]
        name = data[name_rva + 4:name_rva + 4 + ln].decode("utf-16le").split("\\")[-1]
        mods.append((base, size, name))

def where(a):
    for base, size, name in mods:
        if base <= a < base + size:
            return f"{name}+{a - base:#x}"
    return f"{a:#x}"

_, rva = streams[6]   # ExceptionStream
tid = struct.unpack_from("<I", data, rva)[0]
code, flags, rec, addr = struct.unpack_from("<IIQQ", data, rva + 8)
nparams = struct.unpack_from("<I", data, rva + 8 + 24)[0]
params = struct.unpack_from("<15Q", data, rva + 8 + 32)
print(f"exception {code:#x} at {where(addr)} params {[hex(p) for p in params[:nparams]]}")
ctx_size, ctx_rva = struct.unpack_from("<II", data, rva + 8 + 152)
ctx = data[ctx_rva:ctx_rva + ctx_size]
names = "rax rcx rdx rbx rsp rbp rsi rdi r8 r9 r10 r11 r12 r13 r14 r15".split()
regs = struct.unpack_from("<16Q", ctx, 0x78)
rip = struct.unpack_from("<Q", ctx, 0xf8)[0]
print("rip", where(rip))
for n, v in zip(names, regs):
    print(f"  {n} {v:#018x} {where(v) if mods and any(b <= v < b + s for b, s, _ in mods) else ''}")

# Stack: the thread's stack memory from the memory list, scanned for addresses inside modules.
rsp = regs[4]
for t in (5, 9):   # MemoryListStream / Memory64ListStream (only the plain list is handled)
    if t != 5 or t not in streams:
        continue
    _, mrva = streams[5]
    n = struct.unpack_from("<I", data, mrva)[0]
    for i in range(n):
        start, size, drva = struct.unpack_from("<QII", data, mrva + 4 + i * 16)
        if start <= rsp < start + size:
            off = rsp - start
            hits = 0
            while off + 8 <= size and hits < 25:
                v = struct.unpack_from("<Q", data, drva + off)[0]
                w = where(v)
                if "+" in w:
                    print(f"  [rsp+{off - (rsp - start):#x}] {w}")
                    hits += 1
                off += 8
