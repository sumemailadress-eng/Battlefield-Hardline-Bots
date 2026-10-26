import os as _os
def _server():   # the Hardline server folder, from Thugbot-Paths.ini (Setup-Thugbot.bat)
    ini = _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), "..", "Thugbot-Paths.ini")
    for line in open(ini, encoding="ascii", errors="replace"):
        if line.lower().startswith("server="): return line.split("=", 1)[1].strip()
    raise SystemExit("Server folder not set - run Setup-Thugbot.bat")
_ROOT = _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), "..")
import zlib
names = open(_os.path.join(_ROOT, "re", "eia_names.txt"), encoding="ascii").read().split()
targets = {0xe5a74ae0:"fwd+",0xd2620df7:"fwd-",0x8d79a3e7:"yaw",0x403a2bdf:"strafe",0x3ff1a9ca:1,0x3ca9660a:2,0xd309c1ce:4,0x3caf8530:"5/6",0x3cb210df:7,0x4370acd9:8,0x1f860974:9,0xa188655c:10,
           0x251695e4:11,0x5babcc60:12,0x66dded9a:13,0x429ba0eb:14,0xf7b383e3:15,0xbbb7a2c3:16,0x49038def:17,0x4f144ab7:18,0x5048b09c:19,0x0712da98:20}
def fb(s):
    h = 5381
    for c in s: h = ((h * 33) ^ c) & 0xffffffff
    return h
def fbadd(s):
    h = 5381
    for c in s: h = (h * 33 + c) & 0xffffffff
    return h
def fnv1(s):
    h = 0x811c9dc5
    for c in s: h = ((h * 0x01000193) & 0xffffffff) ^ c
    return h
def fnv1a(s):
    h = 0x811c9dc5
    for c in s: h = ((h ^ c) * 0x01000193) & 0xffffffff
    return h
def crc(s): return zlib.crc32(s) & 0xffffffff
found = {}
for fn in (fb, fbadd, fnv1, fnv1a, crc):
    for n in names:
        for v in (n, n.lower(), n[7:] if n.startswith("Concept") else n, (n[7:] if n.startswith("Concept") else n).lower()):
            h = fn(v.encode())
            if h in targets: found[targets[h]] = (fn.__name__, v)
for k, v in sorted(found.items(), key=lambda x: str(x[0])): print(k, v)
