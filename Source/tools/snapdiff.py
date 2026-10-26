"""Diffs Thugbot_snapN.bin files: floats that stay put while the bot stands (snap0 == snap1) and change while it
moves (snap2..4). Usage: python snapdiff.py <dir> [lo hi]  - prints offset, region (soldier/player), values."""
import struct, sys, os

d = sys.argv[1]
lo, hi = (float(sys.argv[2]), float(sys.argv[3])) if len(sys.argv) > 3 else (-1e9, 1e9)
snaps = [open(os.path.join(d, f"Thugbot_snap{i}.bin"), "rb").read() for i in range(5)]
n = len(snaps[0]) // 4
vals = [struct.unpack(f"<{n}f", s) for s in snaps]
for i in range(n):
    v = [x[i] for x in vals]
    if any(x != x or abs(x) > 1e6 for x in v):
        continue
    if v[0] != v[1] or not (v[2] != v[3] != v[4]):
        continue
    if not all(lo <= x <= hi for x in v):
        continue
    off = i * 4
    region = "soldier" if off < 0x2000 else "player"
    print(f"{region} +{off % 0x2000:#06x}  " + "  ".join(f"{x:9.3f}" for x in v))
