import os as _os
def _server():   # the Hardline server folder, from Thugbot-Paths.ini (Setup-Thugbot.bat)
    ini = _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), "..", "Thugbot-Paths.ini")
    for line in open(ini, encoding="ascii", errors="replace"):
        if line.lower().startswith("server="): return line.split("=", 1)[1].strip()
    raise SystemExit("Server folder not set - run Setup-Thugbot.bat")
_ROOT = _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), "..")
import struct, os, math
d = _server()
S = [open(os.path.join(d, f"Thugbot_snap{i}.bin"), "rb").read() for i in range(5)]
for s in S: print("pos", struct.unpack_from("<3f", s, 0x640), "vel", struct.unpack_from("<3f", s, 0x660))
n = len(S[0]) // 4
V = [struct.unpack(f"<{n}f", s) for s in S]
for i in range(n):
    v = [x[i] for x in V]
    if any(x != x or abs(x) > 7 for x in v): continue
    if len({round(x, 4) for x in v[2:]}) < 3: continue
    off = i * 4
    print(("soldier" if off < 0x2000 else "player"), hex(off % 0x2000), " ".join(f"{x:8.3f}" for x in v))
