#!/usr/bin/env python3
"""Read-only reader for Frostbite 3 toc / sb / cat / cas files (BF4, Hardline).
  py fb_bundle.py list <file.toc> [regex]     names of the ebx / res / chunk entries in a level's bundles
"""
import struct, sys, re, os, io

def unxor(path):
    with open(path, 'rb') as f:
        magic = f.read(4)
        if magic[:3] == b'\x00\xd1\xce':
            f.seek(296); key = f.read(260); data = f.read()
            if not any(key): return data   # (00d1ce03: not obfuscated, only signed)
            kb = bytes(k ^ 0x7b for k in key[:257])
            n = len(data); stream = (kb * (n // 257 + 1))[:n]
            return bytes(a ^ b for a, b in zip(data, stream)) if n < 20_000_000 else (int.from_bytes(data, 'little') ^ int.from_bytes(stream, 'little')).to_bytes(n, 'little')
        f.seek(0); return f.read()

def leb(f):
    r = s = 0
    while True:
        b = f.read(1)[0]; r |= (b & 0x7f) << s; s += 7
        if not b & 0x80: return r

def cstr(f):
    o = bytearray()
    while True:
        b = f.read(1)
        if not b or b == b'\0': return o.decode('latin1')
        o += b

def obj(f):
    h = f.read(1)[0]; t = h & 0x1f; flags = h >> 5
    if t == 0: return '', None, True
    name = '' if flags & 4 else cstr(f)
    if t == 0: return name, None, True
    if t == 1:
        leb(f); out = []
        while True:
            _, v, end = obj(f)
            if end: break
            out.append(v)
        return name, out, False
    if t == 2:
        leb(f); out = {}
        while True:
            k, v, end = obj(f)
            if end: break
            out[k] = v
        return name, out, False
    if t == 5: return name, f.read(8), False
    if t == 6: return name, f.read(1)[0], False
    if t == 7: n = leb(f); return name, f.read(n)[:-1].decode('latin1'), False
    if t == 8: return name, struct.unpack('<i', f.read(4))[0], False
    if t == 9: return name, struct.unpack('<q', f.read(8))[0], False
    if t == 0xf: return name, f.read(16), False
    if t == 0x10: return name, f.read(20), False
    if t == 0x13: n = leb(f); return name, f.read(n), False
    raise ValueError('type %#x at %#x' % (t, f.tell()))

def toc(path):
    return obj(io.BytesIO(unxor(path)))[1]

def bundles(tocpath):
    """Yields (bundle id, bundle dict) for a level's toc + sb."""
    t = toc(tocpath); sbp = tocpath[:-4] + '.sb'
    cas = t.get('cas')
    sb = open(sbp, 'rb').read()
    if sb[:3] == b'\x00\xd1\xce': sb = unxor(sbp)
    for b in t.get('bundles', []):
        off, size = b['offset'], b['size']
        if cas:
            yield b['id'], obj(io.BytesIO(sb[off:off + size]))[1]
        else:
            yield b['id'], {'noncas': True, 'offset': off, 'size': size}

if __name__ == '__main__':
    if sys.argv[1] == 'list':
        rx = re.compile(sys.argv[3], re.I) if len(sys.argv) > 3 else None
        t = toc(sys.argv[2]); print({k: (len(v) if isinstance(v, list) else v) for k, v in t.items() if k != 'chunkMeta'})
        for bid, b in bundles(sys.argv[2]):
            names = []
            for kind in ('ebx', 'res'):
                for e in b.get(kind, []) or []:
                    nm = e.get('name', '')
                    if not rx or rx.search(nm): names.append((kind, nm, e.get('size'), e.get('originalSize'), e.get('resType')))
            if names or not rx:
                print('==', bid, 'ebx', len(b.get('ebx', []) or []), 'res', len(b.get('res', []) or []), 'chunks', len(b.get('chunks', []) or []))
                for x in names[:60]: print('  ', x)
