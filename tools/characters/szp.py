"""Yaz0 / Yay0 decompression and a RARC (.szp / .arc) reader (used by make_character_pack.py)."""
import struct, sys, hashlib
sys.path.insert(0, '.')
def yaz0(b):
    assert b[:4] == b'Yaz0'
    n = struct.unpack('>I', b[4:8])[0]; out = bytearray(); p = 16
    while len(out) < n:
        code = b[p]; p += 1
        for i in range(8):
            if len(out) >= n: break
            if code & (0x80 >> i): out.append(b[p]); p += 1
            else:
                b1, b2 = b[p], b[p + 1]; p += 2
                dist = ((b1 & 15) << 8 | b2) + 1; ln = b1 >> 4
                if ln == 0: ln = b[p] + 18; p += 1
                else: ln += 2
                s = len(out) - dist
                for k in range(ln): out.append(out[s + k])
    return bytes(out)
def yay0(b):
    size, linkoff, chunkoff = struct.unpack('>III', b[4:16])
    maskp, linkp, chunkp = 16, linkoff, chunkoff
    out = bytearray(); mask = 0; bits = 0
    while len(out) < size:
        if bits == 0: mask = struct.unpack('>I', b[maskp:maskp + 4])[0]; maskp += 4; bits = 32
        if mask & 0x80000000: out.append(b[chunkp]); chunkp += 1
        else:
            link = struct.unpack('>H', b[linkp:linkp + 2])[0]; linkp += 2
            dist = (link & 0xFFF) + 1; cnt = link >> 12
            if cnt == 0: cnt = b[chunkp] + 18; chunkp += 1
            else: cnt += 2
            s0 = len(out) - dist
            for k in range(cnt): out.append(out[s0 + k])
        mask = (mask << 1) & 0xFFFFFFFF; bits -= 1
    return bytes(out)
def rarc(d):
    assert d[:4] == b'RARC', d[:4]
    data_off = struct.unpack('>I', d[0xC:0x10])[0] + 0x20
    ndir, dir_off, nfile, file_off, strsz, stroff = struct.unpack('>6I', d[0x20:0x38])
    dir_off += 0x20; file_off += 0x20; stroff += 0x20
    def name(o): e = d.find(b'\0', stroff + o); return d[stroff + o:e].decode('latin1')
    dirs = []
    for i in range(ndir):
        t, no, h, cnt, first = struct.unpack('>4sIHHI', d[dir_off + i * 16:dir_off + i * 16 + 16]); dirs.append((name(no), cnt, first))
    out = {}
    def walk(di, path):
        nm, cnt, first = dirs[di]
        for j in range(first, first + cnt):
            fid, hh, flags, no, a0, sz, pad = struct.unpack('>HHHHIII', d[file_off + j * 20:file_off + j * 20 + 20])
            fn = name(no)
            if flags & 0x0200:
                if fn not in ('.', '..'): walk(a0, path + fn + '/')
            else: out[path + fn] = d[data_off + a0:data_off + a0 + sz]
    walk(0, dirs[0][0] + '/')
    return out
def load(iso_files, m, path):
    a, b = iso_files[path]; raw = bytes(m[a:a + b])
    if raw[:4] == b'Yaz0': raw = yaz0(raw)
    elif raw[:4] == b'Yay0': raw = yay0(raw)
    return rarc(raw)
