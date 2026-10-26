import os as _os
def _server():   # the Hardline server folder, from Thugbot-Paths.ini (Setup-Thugbot.bat)
    ini = _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), "..", "Thugbot-Paths.ini")
    for line in open(ini, encoding="ascii", errors="replace"):
        if line.lower().startswith("server="): return line.split("=", 1)[1].strip()
    raise SystemExit("Server folder not set - run Setup-Thugbot.bat")
_ROOT = _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), "..")
import struct
d = _os.path.join(_server(), "Thugbot_players.bin")
blob = open(d, "rb").read(); N = len(blob) // 0x2000
teams = dict(l.rstrip("\n").split("\t") for l in open(_os.path.join(_ROOT, "re", "teams.txt"), encoding="ascii") if "\t" in l)
blocks = [blob[i*0x2000:(i+1)*0x2000] for i in range(N)]
tb = []
for b in blocks:
    t = None
    for name, team in teams.items():
        k = b.find(name.encode())
        if name and k >= 0: t = int(team); print("name", name, "at", hex(k), "team", t); break
    tb.append(t)
known = [(b, t) for b, t in zip(blocks, tb) if t]
for off in range(0, 0x2000, 4):
    if all(struct.unpack_from("<i", b, off)[0] == t for b, t in known) and len({t for _, t in known}) > 1:
        print("team field candidate +%#x" % off)
