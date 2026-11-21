#!/usr/bin/env python3
"""Reads each map's top-down minimap image out of the game files (read only) for the 3D map.
  py fb_minimap.py <game folder> <out folder> <map> [<map> ...]
Writes <out>\\<map>.jpg and <out>\\<map>.json ({"x0","z0","x1","z1"}: the world rectangle the image covers; image rows run with +z).
These images are the game's own: they stay on this PC and do not go into releases."""
import sys, os, re, struct, zlib, json
from io import BytesIO
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fb_bundle import toc, obj, unxor
from PIL import Image

def lz4_block(src):
    out = bytearray(); i = 0; n = len(src)
    while i < n:
        tok = src[i]; i += 1
        lit = tok >> 4
        if lit == 15:
            while True:
                x = src[i]; i += 1; lit += x
                if x != 255: break
        out += src[i:i + lit]; i += lit
        if i >= n: break
        off = src[i] | src[i + 1] << 8; i += 2
        m = tok & 15
        if m == 15:
            while True:
                x = src[i]; i += 1; m += x
                if x != 255: break
        m += 4; st = len(out) - off
        if off >= m: out += out[st:st + m]
        else:
            for k in range(m): out.append(out[st + k])
    return bytes(out)

def decompress(b):
    out = bytearray(); o = 0
    while o + 8 <= len(b):
        un, hdr = struct.unpack_from('>II', b, o); o += 8
        typ = hdr >> 16; size = hdr & 0xffff
        if typ == 0x0270: out += zlib.decompress(b[o:o + size]); o += size
        elif typ == 0x0970: out += lz4_block(b[o:o + size]); o += size
        elif typ in (0x0070, 0x0071): n = un if un else size; out += b[o:o + n]; o += n
        else: raise ValueError('compression %#x' % typ)
    return bytes(out)

class Store:
    def __init__(self, root):
        self.cat = {}; self.tocs = {}
        for dp, dn, fn in os.walk(root):
            low = dp.lower().replace('\\', '/')
            if 'cas.cat' in fn:
                d = open(os.path.join(dp, 'cas.cat'), 'rb').read()
                o = d.find(b'NyanNyanNyanNyan'); o = o + 16 if o >= 0 else 0
                while o + 32 <= len(d):
                    off, size, idx = struct.unpack_from('<III', d, o + 20)
                    self.cat[d[o:o + 20]] = (dp, idx, off, size); o += 32
            if '/win32' in low:
                for f in fn:
                    if f.lower().endswith('.toc'): self.tocs.setdefault(f[:-4].lower(), []).append(os.path.join(dp, f))
    def read(self, sha):
        base, idx, off, size = self.cat[bytes(sha)]
        with open(os.path.join(base, 'cas_%02d.cas' % idx), 'rb') as f:
            f.seek(off); return decompress(f.read(size))

def skip_blocks(sb, o, want):
    """Where a run of compressed blocks that unpacks to want bytes ends, from o."""
    got = 0
    while got < want:
        un, hdr = struct.unpack_from('>II', sb, o); o += 8
        typ = hdr >> 16; size = hdr & 0xffff
        if typ in (0x0070, 0x0071): size = un if un else size
        o += size; got += un
    return o

def binary_bundle(sb, off, res, ebx, chunks, path):
    """A DLC map's bundle: no catalog, everything sits in the .sb, big-endian header then the data."""
    meta, magic, total, nebx, nres, nch, strOff, cmOff, cmSize = struct.unpack_from('>9I', sb, off)
    if magic != 0x9d798ed5: return
    b = off + 4
    p = b + 32 + 20 * total
    ents = []
    strs = b + strOff
    def name_at(o):
        e = sb.index(b'\0', strs + o); return sb[strs + o:e].decode('latin1')
    for k in range(nebx):
        n, size = struct.unpack_from('>II', sb, p); p += 8; ents.append(('ebx', name_at(n), size))
    for k in range(nres):
        n, size = struct.unpack_from('>II', sb, p); p += 8; ents.append(('res', name_at(n), size))
    p += 4 * nres + 16 * nres + 8 * nres
    for k in range(nch):
        g = sb[p:p + 16]; lo, ls = struct.unpack_from('>II', sb, p + 16); p += 24
        ents.append(('chunk', g, (lo & 0xffff) | ls))
    o = off + 4 + meta
    for kind, nm, size in ents:
        e = skip_blocks(sb, o, size)
        if kind == 'chunk': chunks[nm] = {'blob': (path, o, e - o)}; chunks[nm[3::-1] + nm[5:3:-1] + nm[7:5:-1] + nm[8:]] = chunks[nm]
        elif 'minimaps/' in nm: (res if kind == 'res' else ebx)[nm] = {'name': nm, 'blob': (path, o, e - o)}
        o = e

def fetch(st, e):
    if 'blob' in e:
        path, o, n = e['blob']
        with open(path, 'rb') as f:
            f.seek(o); return decompress(f.read(n))
    return st.read(e['sha1'])

def level_contents(path):
    """All minimap res / ebx entries and all chunks of a level toc + sb."""
    res = {}; ebx = {}; chunks = {}
    t = toc(path)
    for c in t.get('chunks', []) or []:
        if 'sha1' in c: chunks[bytes(c['id'])] = c
    sb = open(path[:-4] + '.sb', 'rb').read()
    if sb[:3] == b'\x00\xd1\xce' and any(sb[296:556]): sb = unxor(path[:-4] + '.sb')
    for b in t.get('bundles', []) or []:
        if not t.get('cas'):
            try: binary_bundle(sb, b['offset'], res, ebx, chunks, path[:-4] + '.sb')
            except Exception as ex: print('   bundle', b.get('id'), repr(ex)[:80])
            continue
        try: d = obj(BytesIO(sb[b['offset']:b['offset'] + b['size']]))[1]
        except Exception: continue
        if not isinstance(d, dict): continue
        for c in d.get('chunks', []) or []:
            if 'sha1' in c: chunks[bytes(c['id'])] = c
        for e in d.get('res', []) or []:
            if 'minimaps/' in e.get('name', ''): res[e['name']] = e
        for e in d.get('ebx', []) or []:
            if 'minimaps/' in e.get('name', ''): ebx[e['name']] = e
    return res, ebx, chunks

def dds(w, h, fourcc, body):
    head = b'DDS ' + struct.pack('<7I', 124, 0x1007, h, w, len(body), 0, 1) + b'\0' * 44
    head += struct.pack('<2I', 32, 4) + fourcc + b'\0' * 20 + struct.pack('<5I', 0x1000, 0, 0, 0, 0)
    return head + body

def tile_image(st, chunks, resdata):
    w, h = struct.unpack_from('<HH', resdata, 0x12)
    guid = resdata[0x1c:0x2c]; size0 = struct.unpack_from('<I', resdata, 0x2c)[0]
    c = chunks.get(guid)
    if c is None: return None
    body = fetch(st, c)[:size0]
    four = b'DXT1' if size0 / (w * h) <= 0.5 else b'DXT5'
    return Image.open(BytesIO(dds(w, h, four, body))).convert('RGB')

def rect_from_ebx(d):
    """The placement: three unit axis rows, then the position and half sizes (Vec3s, 16 bytes each)."""
    best = None
    for o in range(0, len(d) - 80, 4):
        r = [struct.unpack_from('<3f', d, o + k * 16) for k in range(3)]
        if not all(abs(sum(a * a for a in v) - 1) < 0.001 and max(abs(a) for a in v) > 0.999 for v in r): continue
        c = struct.unpack_from('<3f', d, o + 48); h = struct.unpack_from('<3f', d, o + 64)
        if 50 < h[0] < 20000 and 50 < h[2] < 20000 and all(abs(a) < 50000 for a in c):
            r2 = dict(x0=c[0] - h[0], x1=c[0] + h[0], z0=c[2] - h[2], z1=c[2] + h[2])
            if best is None or h[0] * h[2] > (best['x1'] - best['x0']) * (best['z1'] - best['z0']) / 4: best = r2   # (small volumes come first on some maps: the biggest is the whole map)
    return best

def one_map(st, name, out):
    paths = st.tocs.get(name.lower(), [])
    if not paths: return 'no level file'
    why = 'no minimap'
    for p in paths:
        try: res, ebx, chunks = level_contents(p)
        except Exception as e: why = 'read failed: ' + repr(e)[:80]; continue
        if not res: continue
        # chunk tocs of the map (Hardline keeps them apart: <map>chunks.toc)
        for extra in st.tocs.get(name.lower() + 'chunks', []):
            try:
                for c in toc(extra).get('chunks', []) or []:
                    if 'sha1' in c: chunks[bytes(c['id'])] = c
            except Exception: pass
        tiles = {}
        for nm, e in res.items():
            m = re.search(r'/([^/]+?)_mip(\d+)_(\d+)_(\d+)$', nm)
            if m: tiles[(m[1], int(m[2]), int(m[3]), int(m[4]))] = e
        sets = sorted({k[0] for k in tiles}, key=lambda a: (('levolution' in a), -sum(1 for k in tiles if k[0] == a)))
        for a in sets:
            top = max(k[1] for k in tiles if k[0] == a)
            keys = [k for k in tiles if k[0] == a and k[1] == top]
            canvas = None; T = 0; xs = [k[2] for k in keys]; ys = [k[3] for k in keys]; got = 0
            for k in keys:
                try: im = tile_image(st, chunks, fetch(st, tiles[k]))
                except (KeyError, ValueError, struct.error): im = None
                if im is None: continue
                if canvas is None: T = im.width; canvas = Image.new('RGB', ((max(xs) + 1) * T, (max(ys) + 1) * T))
                canvas.paste(im, (k[2] * T, k[3] * T)); got += 1
            if not got: why = 'tile data not found'; continue
            rect = None
            for nm, e in ebx.items():
                if nm.endswith('minimap2dtexture') and (a.split('(')[0] in nm or a.startswith('__default')):
                    r2 = rect_from_ebx(fetch(st, e))
                    if r2 and (not rect or r2['x1'] - r2['x0'] > rect['x1'] - rect['x0']): rect = r2
            if not rect:   # (a map whose image set has its own name: any placement in the map)
                for nm, e in ebx.items():
                    if nm.endswith('minimap2dtexture') and 'levolution' not in nm:
                        rect = rect_from_ebx(fetch(st, e))
                        if rect: break
            if not rect: why = 'no placement'; continue
            if canvas.width > 2048: canvas = canvas.resize((2048, 2048 * canvas.height // canvas.width))
            canvas.save(os.path.join(out, name + '.jpg'), quality=82)
            rect['lines'] = sum(canvas.convert('L').resize((64, 64)).getdata()) / 4096 < 15   # (Hardline's are thin lines on black, not pictures)
            json.dump(rect, open(os.path.join(out, name + '.json'), 'w'))
            return 'ok %s mip %d, %d x %d, x %.0f..%.0f z %.0f..%.0f' % (a, top, canvas.width, canvas.height, rect['x0'], rect['x1'], rect['z0'], rect['z1'])
    return why

if __name__ == '__main__':
    root, out = sys.argv[1:3]; os.makedirs(out, exist_ok=True)
    st = Store(root); print('catalog', len(st.cat), 'tocs', len(st.tocs), flush=True)
    for nm in sys.argv[3:]:
        print(nm, one_map(st, nm, out), flush=True)
