import os as _os
def _server():   # the Hardline server folder, from Thugbot-Paths.ini (Setup-Thugbot.bat)
    ini = _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), "..", "Thugbot-Paths.ini")
    for line in open(ini, encoding="ascii", errors="replace"):
        if line.lower().startswith("server="): return line.split("=", 1)[1].strip()
    raise SystemExit("Server folder not set - run Setup-Thugbot.bat")
_ROOT = _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), "..")
import struct
d = _server()
for i in (0, 1):
    s = open(d + rf"\Thugbot_snap{i}.bin", "rb").read()
    print(i, "pos", [round(x,2) for x in struct.unpack_from("<3f", s, 0x640)], "rot540", [round(x,2) for x in struct.unpack_from("<12f", s, 0x530)], "58c", struct.unpack_from("<f", s, 0x58c))
    print("   vtable", hex(struct.unpack_from("<Q", s, 0)[0]))
