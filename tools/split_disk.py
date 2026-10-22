#!/usr/bin/env python3
"""Split a large HFS disk image into smaller ones (Raspberry Pi port: a
bootable System disk for drive 1 plus "games" disks for drive 2, sized to
fit the storage at hand).

  # bootable System disk: the System Folder and anything else wanted, minus
  # the big folders that go to other disks
  python3 tools/split_disk.py system BIG.DSK OUT.dsk --name "System" \
      --drop "Desktop Folder:Games" --drop "Desktop Folder:Applications" \
      --size 48M

  # ... optionally with extensions/control panels moved to the "(Disabled)"
  # folders, as Extensions Manager does (they can be re-enabled there):
  #   --disable "Extensions:Eyeballs" --disable "Control Panels:After Dark 2.0"

  # data disks: the items of one folder, biggest first, packed into as many
  # images of --size as needed (OUT_1.dsk, OUT_2.dsk, ...)
  python3 tools/split_disk.py pack BIG.DSK OUT.dsk --name "Games" \
      --from "Desktop Folder:Games" --size 50M [--max-disks 1]

Paths are colon-separated, as in the Mac OS. Requires machfs (see
build_disk.py; --pylib DIR adds an unpacked copy to sys.path). The source
image is only read.
"""
import argparse
import os
import struct
import sys


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('mode', choices=('system', 'pack'))
    ap.add_argument('src')
    ap.add_argument('out')
    ap.add_argument('--name', required=True, help='volume name (packs get " 1", " 2", ...)')
    ap.add_argument('--size', default='48M', help='image size, e.g. 48M')
    ap.add_argument('--drop', action='append', default=[], help='system: path to leave out')
    ap.add_argument('--disable', action='append', default=[],
                    help='system: "Extensions:NAME" or "Control Panels:NAME" (System Folder) '
                         'to move into the matching "(Disabled)" folder')
    ap.add_argument('--from', dest='src_folder', help='pack: folder whose items are packed')
    ap.add_argument('--max-disks', type=int, default=0, help='pack: stop after N images')
    ap.add_argument('--pylib', help='extra sys.path entry holding machfs/macresources')
    a = ap.parse_args()
    if a.pylib:
        sys.path.insert(0, a.pylib)
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import machfs
    import build_disk as bd

    size = bd.parse_size(a.size)
    src = bd.read_volume(machfs, a.src)

    def write(vol, path, bootable):
        img = bytearray(vol.write(size=size, align=512, desktopdb=True, bootable=bootable))
        if bootable:                       # bless the System Folder (MDB + alternate MDB)
            sfid = bd.catalog_dirs(img)[('system folder',)][0]
            for mdb in (1024, len(img) - 1024):
                struct.pack_into('>L', img, mdb + 0x5C, sfid)
        with open(path, 'wb') as fh:
            fh.write(img)
        free = struct.unpack_from('>H', img, 1024 + 0x22)[0] * \
            struct.unpack_from('>L', img, 1024 + 0x14)[0]
        print(f'wrote {path}: {len(img) // 1048576} MB, {free // 1024} K free')

    if a.mode == 'system':
        vol = machfs.Volume()
        vol.name = a.name
        vol.crdate = vol.mddate = src.crdate
        for name, node in src.items():
            vol[name] = node
        for p in a.drop:
            bd.del_path(vol, tuple(p.split(':')))
            print(f'  dropped {p}')
        sf = vol['System Folder']
        for p in a.disable:
            folder, name = p.split(':', 1)
            if folder not in sf or name not in sf[folder]:
                print(f'  not found: {p}')
                continue
            dis = folder + ' (Disabled)'
            if dis not in sf:
                sf[dis] = machfs.Folder()
            sf[dis][name] = sf[folder][name]
            del sf[folder][name]
            print(f'  disabled {p}')
        write(vol, a.out, True)
        return

    if not a.src_folder:
        raise SystemExit('pack needs --from FOLDER')
    folder = bd.get_path(src, tuple(a.src_folder.split(':')))
    items = sorted(folder.items(), key=lambda kv: -bd.node_size(machfs, kv[1]))
    budget = size * 0.93 - 256 * 1024        # catalog/extents/desktop DB overhead
    disks = []
    for name, node in items:                 # first fit, biggest items first
        sz = bd.node_size(machfs, node)
        for d in disks:
            if d[0] + sz <= budget:
                d[0] += sz
                d[1].append((name, node))
                break
        else:
            disks.append([sz, [(name, node)]])
    if a.max_disks:
        disks = disks[:a.max_disks]
    base, ext = os.path.splitext(a.out)
    for i, (used, members) in enumerate(disks, 1):
        vol = machfs.Volume()
        vol.name = f'{a.name} {i}' if len(disks) > 1 else a.name
        vol.crdate = vol.mddate = src.crdate
        for name, node in sorted(members):
            vol[name] = node
        out = f'{base}_{i}{ext}' if len(disks) > 1 else a.out
        try:
            write(vol, out, False)
        except (machfs.main.OutOfSpaceError, ValueError):
            raise SystemExit(f'{out}: does not fit, use a larger --size')
        print(f'  {len(members)} items, {used / 1048576:.1f} MB: '
              + ', '.join(n for n, _ in sorted(members)))


if __name__ == '__main__':
    main()
