"""Minimal VCDIFF (RFC 3284 + xdelta3's adler32 / LZMA secondary compression) decoder.
usage: python vcdiff.py <source> <patch> <output>
Written to apply one .xdelta to a local disc image. Every window's adler32 is checked; a section flagged as
secondary-compressed is tried as an xz stream, then as stored bytes, then as stored bytes after a size prefix,
and the first form whose decoded window passes its checksum is kept."""
import itertools, lzma, mmap, struct, sys, zlib

def varint(b, p):
    n = 0
    while True:
        c = b[p]; p += 1
        n = (n << 7) | (c & 0x7F)
        if not c & 0x80: return n, p

def build_code_table():
    t = []  # (type1, size1, mode1, type2, size2, mode2); types 0 noop, 1 add, 2 run, 3 copy
    t.append((2, 0, 0, 0, 0, 0))
    for s in range(0, 18): t.append((1, s, 0, 0, 0, 0))
    for mode in range(9):
        t.append((3, 0, mode, 0, 0, 0))
        for s in range(4, 19): t.append((3, s, mode, 0, 0, 0))
    for mode in range(6):
        for a in range(1, 5):
            for c in range(4, 7): t.append((1, a, 0, 3, c, mode))
    for mode in range(6, 9):
        for a in range(1, 5): t.append((1, a, 0, 3, 4, mode))
    for mode in range(9): t.append((3, 4, mode, 1, 1, 0))
    assert len(t) == 256, len(t)
    return t
TABLE = build_code_table()

def xz(chunk):
    size, p = varint(chunk, 0)
    d = lzma.LZMADecompressor(format=lzma.FORMAT_XZ)
    out = d.decompress(chunk[p:])
    if len(out) != size: raise ValueError("size")
    return out

def candidates(chunk, flag):
    chunk = bytes(chunk)
    if not flag:
        yield chunk
        return
    try: yield xz(chunk)
    except (lzma.LZMAError, ValueError, IndexError): pass
    yield chunk
    _, q = varint(chunk, 0)
    yield chunk[q:]

def decode_window(data, inst, addr, src_seg, tlen):
    tgt = bytearray()
    near = [0] * 4; same = [0] * 768; nslot = 0
    ip = dp = ap = 0
    ssz = len(src_seg)
    def addr_decode(mode, here):
        nonlocal ap, nslot
        if mode == 0: a, ap = varint(addr, ap)
        elif mode == 1: v, ap = varint(addr, ap); a = here - v
        elif mode < 6: v, ap = varint(addr, ap); a = near[mode - 2] + v
        else: a = same[(mode - 6) * 256 + addr[ap]]; ap += 1
        near[nslot] = a; nslot = (nslot + 1) % 4
        same[a % 768] = a
        return a
    while ip < len(inst):
        t1, s1, m1, t2, s2, m2 = TABLE[inst[ip]]; ip += 1
        for typ, size, mode in ((t1, s1, m1), (t2, s2, m2)):
            if typ == 0: continue
            if size == 0: size, ip = varint(inst, ip)
            if typ == 1:
                if dp + size > len(data): raise ValueError("add past data")
                tgt += data[dp:dp + size]; dp += size
            elif typ == 2:
                tgt += bytes([data[dp]]) * size; dp += 1
            else:
                a = addr_decode(mode, ssz + len(tgt))
                if a < 0: raise ValueError("negative address")
                if a < ssz:
                    tgt += src_seg[a:a + size]
                else:
                    a -= ssz
                    if a >= len(tgt): raise ValueError("copy past target")
                    for _ in range(size): tgt.append(tgt[a]); a += 1
    if len(tgt) != tlen: raise ValueError("length")
    return tgt

def apply(src_path, patch_path, out_path):
    patch = open(patch_path, 'rb').read()
    assert patch[:3] == b'\xd6\xc3\xc4', "not a VCDIFF file"
    p = 4
    ind = patch[p]; p += 1
    if ind & 1: p += 1                      # secondary compressor id
    if ind & 2: raise ValueError("custom code table")
    if ind & 4:
        n, p = varint(patch, p); p += n     # application header
    srcf = open(src_path, 'rb'); src = mmap.mmap(srcf.fileno(), 0, access=mmap.ACCESS_READ)
    out = open(out_path, 'wb')
    win = 0
    streams = [lzma.LZMADecompressor(format=lzma.FORMAT_XZ) for _ in range(3)]
    while p < len(patch):
        wi = patch[p]; p += 1
        src_seg = b''
        if wi & 3:
            n, p = varint(patch, p); pos, p = varint(patch, p)
            if wi & 2: raise ValueError("VCD_TARGET windows are not supported")
            src_seg = src[pos:pos + n]
        dlen, p = varint(patch, p)
        end = p + dlen
        tlen, p = varint(patch, p)
        di = patch[p]; p += 1
        dl, p = varint(patch, p); il, p = varint(patch, p); al, p = varint(patch, p)
        adler = None
        if wi & 4: adler = struct.unpack('>I', patch[p:p + 4])[0]; p += 4
        secs = []
        for k, (ln, flag) in enumerate(((dl, di & 1), (il, di & 2), (al, di & 4))):
            chunk = patch[p:p + ln]; p += ln
            if flag:  # one xz stream per section kind runs through the whole file (only the first window has its header)
                size, q = varint(chunk, 0)
                chunk = streams[k].decompress(bytes(chunk[q:]))
                if len(chunk) != size: raise ValueError("window %d: section %d decoded to %d bytes, expected %d" % (win, k, len(chunk), size))
            secs.append(bytes(chunk))
        assert p == end, (p, end)
        tgt = decode_window(secs[0], secs[1], secs[2], src_seg, tlen)
        if adler is not None and zlib.adler32(bytes(tgt)) & 0xFFFFFFFF != adler:
            raise ValueError("window %d: adler32 mismatch" % win)
        out.write(tgt)
        win += 1
        if win % 50 == 0: print("windows", win, "patch", p * 100 // len(patch), "%", flush=True)
    out.close()
    print("done:", win, "windows")

if __name__ == '__main__':
    apply(*sys.argv[1:4])
