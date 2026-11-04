#!/usr/bin/env python3
"""Build an LmCoop character pack (the playable Luigi and his voice banks) from an .xdelta mod patch.

    python tools/make_character_pack.py --disc GLME01.iso --patch mod.xdelta --out "<dataDir>/characters/wega" --name "Wega's Luigi"

--disc is your own USA disc as a plain ISO (python tools/make_iso.py <extracted dump> GLME01.iso). The patch is
applied in a temporary file; only these files are kept in --out, nothing else of the mod:
  luige.mdl                       the playable Luigi's model (luige/model/luige.mdl inside data/model/luige.arc
                                  inside Game/game_usa.szp)
  LuiSec0_0.aw LuiSec1_0.aw LuiSec2_0.aw JaiInit.aaf    the voice banks
  pack.ini                        name=...
The pack is for local use with your own disc; never distribute it. <dataDir> is the folder the game prints at
startup ("[install] data folder ..."), e.g. %APPDATA%\\Luigi's Mansion Recompiled on Windows.
"""
import argparse, os, shutil, sys, tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent / 'characters'))
import gcm, szp, vcdiff

VOICE_FILES = ['AudioRes/Banks/LuiSec0_0.aw', 'AudioRes/Banks/LuiSec1_0.aw', 'AudioRes/Banks/LuiSec2_0.aw', 'AudioRes/JaiInit.aaf']

def disc_file(files, m, path):
    a, n = files[path]
    return bytes(m[a:a + n])

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--disc', required=True, type=Path)
    ap.add_argument('--patch', required=True, type=Path)
    ap.add_argument('--out', required=True, type=Path)
    ap.add_argument('--name', default='Mod Luigi')
    a = ap.parse_args()
    tmp = tempfile.mkdtemp(prefix='lmcoop-pack-')
    maps = []
    try:
        patched = Path(tmp) / 'patched.iso'
        print('applying the patch (a few minutes)...')
        vcdiff.apply(str(a.disc), str(a.patch), str(patched))
        base_m, base_files, _ = gcm.files(str(a.disc))
        m, files, _ = gcm.files(str(patched))
        maps += [base_m, m]
        a.out.mkdir(parents=True, exist_ok=True)
        game = szp.load(files, m, 'Game/game_usa.szp')
        base_game = szp.load(base_files, base_m, 'Game/game_usa.szp')
        arc = szp.rarc(game['data/model/luige.arc'])
        base_arc = szp.rarc(base_game['data/model/luige.arc'])
        mdl = arc['luige/model/luige.mdl']
        if mdl == base_arc['luige/model/luige.mdl']:
            sys.exit('this patch does not change the playable Luigi (luige.mdl is identical to the disc\'s)')
        (a.out / 'luige.mdl').write_bytes(mdl)
        print('luige.mdl', len(mdl), 'bytes')
        for p in VOICE_FILES:
            data = disc_file(files, m, p)
            (a.out / Path(p).name).write_bytes(data)
            print(Path(p).name, len(data), 'bytes', '(changed)' if data != disc_file(base_files, base_m, p) else '(same as the disc)')
        (a.out / 'pack.ini').write_text('name=%s\n' % a.name)
    finally:
        for mm in maps: mm.close()
        shutil.rmtree(tmp, ignore_errors=True)
    print('pack written to', a.out)

if __name__ == '__main__':
    main()
