"""GameCube disc image: file table reader (used by make_character_pack.py)."""
import hashlib, mmap, struct, sys
def files(path):
    f = open(path, 'rb'); m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
    fst_off, fst_size = struct.unpack('>II', m[0x424:0x42C])
    n = struct.unpack('>I', m[fst_off + 8:fst_off + 12])[0]
    names = fst_off + n * 12
    def name(o):
        e = m.find(b'\0', names + o); return m[names + o:e].decode('latin1')
    out = {}; stack = []  # (end index, path)
    for i in range(1, n):
        e = fst_off + i * 12
        flag = m[e]; no = int.from_bytes(m[e + 1:e + 4], 'big'); a, b = struct.unpack('>II', m[e + 4:e + 12])
        while stack and i >= stack[-1][0]: stack.pop()
        base = '/'.join(p for _, p in stack)
        if flag: stack.append((b, name(no)))
        else: out[(base + '/' + name(no)).lstrip('/')] = (a, b)
    return m, out, (m[0x420:0x424], fst_off, fst_size)
