import os as _os
def _server():   # the Hardline server folder, from Thugbot-Paths.ini (Setup-Thugbot.bat)
    ini = _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), "..", "Thugbot-Paths.ini")
    for line in open(ini, encoding="ascii", errors="replace"):
        if line.lower().startswith("server="): return line.split("=", 1)[1].strip()
    raise SystemExit("Server folder not set - run Setup-Thugbot.bat")
_ROOT = _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), "..")
import struct, math
d = _server()
for i in (0, 1):
    s = open(d + rf"\Thugbot_snap{i}.bin", "rb").read()
    v = struct.unpack_from("<3f", s, 0x660); f = struct.unpack_from("<3f", s, 0x550); r = struct.unpack_from("<3f", s, 0x540)
    a = struct.unpack_from("<f", s, 0x58c)[0]
    print(f"vel {v[0]:.2f},{v[2]:.2f} dir {math.degrees(math.atan2(v[0], v[2])):.0f} | row550 {f[0]:.2f},{f[2]:.2f} dir {math.degrees(math.atan2(f[0], f[2])):.0f} | row540 {r[0]:.2f},{r[2]:.2f} | 58c {a:.3f} ({math.degrees(a)%360:.0f})")
