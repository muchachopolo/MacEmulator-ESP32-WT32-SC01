#!/usr/bin/env python3
"""Build a custom System 7.5.3 boot disk (bare HFS image) for the emulator.

Contains no Apple data: everything is copied at run time from images the
user supplies.

  base   : a bootable System 7.5.3 floppy image (e.g. ROM/OS_750_boot.dsk).
           Its System, Finder, Finder Preferences and boot blocks are used.
  --hd   : a full System 7.5.3 hard-disk image (bare HFS or with an Apple
           partition map). Items in MANIFEST below are copied from it.
  --extra: user apps: MacBinary (.bin) files, HFS images (.dsk/.img/.hfs/
           .image) or folders of them. Copied into EXTRA_DEST.

Size: --size 2880K (default, see SIZE_PROFILES), any "<n>K"/"<n>M"/bytes,
or "floppy" (1440K: drops manifest items, lowest priority first, until the
volume fits). Requires the `machfs` package (pip install machfs); --pylib
adds a directory to sys.path (for an unpacked copy).

Example:
  python3 tools/build_disk.py ROM/OS_750_boot.dsk \
      --hd "/path/System 7.5.3 HD.dsk" --out ROM/custom_753.dsk
"""
import argparse
import os
import struct
import sys

# ---------------------------------------------------------------------------
# Manifest: (priority, source path in the HD image, destination path).
# Paths are tuples of HFS names. Lower priority numbers are dropped first
# when the volume does not fit (floppy profile). A 4th element 'optional'
# means the item is only copied with --include NAME. Edit freely.
# ---------------------------------------------------------------------------
SF = ('System Folder',)
AMI = SF + ('Apple Menu Items',)
CP = SF + ('Control Panels',)

MANIFEST = [
    # Apple menu
    (90, AMI + ('Calculator',),          AMI + ('Calculator',)),
    (85, AMI + ('Key Caps',),            AMI + ('Key Caps',)),
    (80, AMI + ('Note Pad',),            AMI + ('Note Pad',)),
    (40, AMI + ('Stickies',),            AMI + ('Stickies',)),
    (50, AMI + ('Scrapbook',),           AMI + ('Scrapbook',)),
    (50, SF + ('Scrapbook File',),       SF + ('Scrapbook File',)),
    (30, AMI + ('Jigsaw Puzzle',),       AMI + ('Jigsaw Puzzle',)),
    (95, AMI + ('• Shut Down',),    AMI + ('• Shut Down',)),
    # Control panels
    (88, CP + ('General Controls',),     CP + ('General Controls',)),
    (87, CP + ('Date & Time',),          CP + ('Date & Time',)),
    (86, CP + ('Mouse',),                CP + ('Mouse',)),
    (84, CP + ('Keyboard',),             CP + ('Keyboard',)),
    (60, CP + ('Sound',),                CP + ('Sound',)),
    (70, CP + ('Monitors',),             CP + ('Monitors',)),
    (65, CP + ('Memory',),               CP + ('Memory',)),
    (83, CP + ('Views',),                CP + ('Views',)),
    (82, CP + ('Labels',),               CP + ('Labels',)),
    (81, CP + ('Startup Disk',),         CP + ('Startup Disk',)),
    (55, CP + ('Extensions Manager',),   CP + ('Extensions Manager',)),
    # WindowShade's INIT corrupts memory on the Classic build (2 MB, lean
    # System): opening General Controls then crashes. Fine on the LC II.
    # Off by default; add it with --include WindowShade.
    (75, CP + ('WindowShade',),          CP + ('WindowShade',), 'optional'),
    # Applications
    (78, ('Apple Extras', 'SimpleText'), ('Applications', 'SimpleText')),
]

# Aliases created on the new volume: (alias path, target path).
ALIASES = [
    (AMI + ('Control Panels',), CP),
]

# Finder window rectangles (global top, left, bottom, right) for folders,
# so that e.g. all control panels are visible on the 480x320 screen.
FOLDER_WINDOWS = {
    (): (60, 16, 200, 330),
    SF + ('Control Panels',): (40, 6, 312, 474),
    SF + ('Apple Menu Items',): (60, 30, 290, 450),
}

# Items kept from the base floppy (everything else on it is dropped).
BASE_KEEP = [
    SF + ('System',),
    SF + ('Finder',),
    SF + ('Preferences', 'Finder Preferences'),
]
# Empty special folders created in the System Folder.
SYSTEM_SUBFOLDERS = ['Apple Menu Items', 'Control Panels', 'Extensions',
                     'Fonts', 'Preferences', 'Startup Items',
                     'Shutdown Items']
EXTRA_DEST = ('Applications',)
EXTRA_PRIORITY = 20       # user apps go before manifest items in a squeeze

# 2880K = two floppies. Firmware budget: app partition 0x3F0000 (4128768)
# minus ~1.10 MB of code + 512K ROM leaves ~2.89 MiB for the disk.
SIZE_PROFILES = {'default': 2880 * 1024, 'floppy': 1440 * 1024}


def parse_size(s):
    s = s.strip().lower()
    if s in SIZE_PROFILES:
        return SIZE_PROFILES[s]
    mult = 1
    if s.endswith('k'):
        mult, s = 1024, s[:-1]
    elif s.endswith('m'):
        mult, s = 1024 * 1024, s[:-1]
    n = int(float(s) * mult)
    return n - n % 512


# ---------------------------------------------------------------------------
# Image helpers
# ---------------------------------------------------------------------------
def hfs_partitions(img):
    """Yield byte slices of HFS volumes in an image (bare or partitioned)."""
    if img[1024:1026] == b'BD':
        yield img
        return
    if img[:2] == b'ER' and img[512:514] == b'PM':
        nparts = struct.unpack_from('>L', img, 512 + 4)[0]
        for i in range(nparts):
            e = 512 * (1 + i)
            if img[e:e + 2] != b'PM':
                break
            start, cnt = struct.unpack_from('>LL', img, e + 8)
            ptype = img[e + 48:e + 80].split(b'\0')[0]
            if ptype == b'Apple_HFS':
                yield img[start * 512:(start + cnt) * 512]
        return
    # DiskCopy 4.2 header (84 bytes)
    if len(img) > 84 and img[84 + 1024:84 + 1026] == b'BD':
        yield img[84:]


def read_volume(machfs, path):
    img = open(path, 'rb').read()
    for part in hfs_partitions(img):
        v = machfs.Volume()
        v.read(part)
        return v
    raise SystemExit(f'{path}: no HFS volume found')


def node_size(machfs, n):
    if isinstance(n, machfs.File):
        return len(n.data) + len(n.rsrc)
    return sum(node_size(machfs, c) for c in n.values())


def get_path(vol, path):
    n = vol
    for c in path:
        n = n[c]
    return n


def put_path(machfs, vol, path, node):
    parent = vol
    for c in path[:-1]:
        if c not in parent:
            parent[c] = machfs.Folder()
        parent = parent[c]
    parent[path[-1]] = node


def del_path(vol, path):
    parent = get_path(vol, path[:-1])
    del parent[path[-1]]


def parse_macbinary(machfs, data):
    """Return (name, File) from a MacBinary I/II/III blob."""
    if len(data) < 128 or data[0] != 0 or not 1 <= data[1] <= 63:
        raise ValueError('not MacBinary')
    name = data[2:2 + data[1]].decode('mac_roman')
    f = machfs.File()
    f.type, f.creator = data[65:69], data[69:73]
    f.flags = (data[73] << 8) | data[101]
    f.x, f.y = struct.unpack_from('>hh', data, 75)
    dlen, rlen = struct.unpack_from('>LL', data, 83)
    f.crdate, f.mddate = struct.unpack_from('>LL', data, 91)
    sec2 = struct.unpack_from('>H', data, 120)[0]
    off = 128 + ((sec2 + 127) // 128) * 128
    f.data = data[off:off + dlen]
    off += ((dlen + 127) // 128) * 128
    f.rsrc = data[off:off + rlen]
    if len(f.data) != dlen or len(f.rsrc) != rlen:
        raise ValueError('truncated MacBinary')
    f.flags &= ~0x0100            # clear hasBeenInited (fresh desktop)
    return name, f


def collect_extras(machfs, paths):
    """Yield (name, node) for each --extra item."""
    files = []
    for p in paths:
        if os.path.isdir(p):
            for fn in sorted(os.listdir(p)):
                files.append(os.path.join(p, fn))
        else:
            files.append(p)
    for p in files:
        ext = os.path.splitext(p)[1].lower()
        if ext in ('.bin', '.macbin'):
            yield parse_macbinary(machfs, open(p, 'rb').read())
        elif ext in ('.dsk', '.img', '.hfs', '.image', '.dmg'):
            v = read_volume(machfs, p)
            for name, node in v.items():
                if name in ('System Folder', 'Desktop Folder', 'Trash',
                            'Desktop', 'Desktop DB', 'Desktop DF'):
                    continue
                yield name, node
        else:
            print(f'  skip extra {p} (unknown type)')


# ---------------------------------------------------------------------------
def catalog_dirs(img):
    """Map folder path tuple (lowercase) -> (dirID, byte offset of its
    catalog record value) for a bare HFS image."""
    m = img[1024:]
    alsz = struct.unpack_from('>L', m, 0x14)[0]
    alst = struct.unpack_from('>H', m, 0x1C)[0]
    ctsize = struct.unpack_from('>L', m, 0x92)[0]
    exts = [struct.unpack_from('>HH', m, 0x96 + 4 * i) for i in range(3)]
    # byte offsets of the catalog file's bytes (in order)
    spans = [(512 * alst + alsz * a, alsz * b) for a, b in exts if b]

    def foff(pos):
        for base, ln in spans:
            if pos < ln:
                return base + pos
            pos -= ln
        raise ValueError('catalog offset out of range')

    node = lambda n: foff(n * 512)          # nodes never straddle extents
    hdr = node(0)
    nodesize = struct.unpack_from('>H', img, hdr + 14 + 18)[0]
    assert nodesize == 512
    leaf = struct.unpack_from('>L', img, hdr + 14 + 10)[0]
    recs = {}
    while leaf:
        no = node(leaf)
        flink, _, ntype, _, nrecs = struct.unpack_from('>LLbBH', img, no)
        for i in range(nrecs):
            r = no + struct.unpack_from('>H', img, no + 510 - 2 * i)[0]
            kl = img[r]
            if not kl:
                continue
            par, nl = struct.unpack_from('>LB', img, r + 2)
            name = bytes(img[r + 7:r + 7 + nl]).decode('mac_roman').lower()
            v = r + ((2 + kl) & ~1)
            if img[v] == 1:
                recs[(par, name)] = (struct.unpack_from('>L', img, v + 6)[0], v)
        leaf = flink
    root = [v for (par, _), v in recs.items() if par == 1]
    out = {(): root[0] if root else (2, None)}
    pending = True
    while pending:
        pending = False
        for path, (did, _) in list(out.items()):
            for (par, name), val in recs.items():
                if par == did and path + (name,) not in out:
                    out[path + (name,)] = val
                    pending = True
    return out


def copy_folder_info(dst_img, src_img, paths=None):
    """Copy DInfo+DXInfo (window, flags, view) of folders present in both
    images. `paths` limits it to those folder paths."""
    sd, dd = catalog_dirs(src_img), catalog_dirs(dst_img)
    for path, (did, voff) in dd.items():
        if voff is None or path not in sd:
            continue
        if paths is not None and path not in paths:
            continue
        soff = sd[path][1]
        # dirUsrInfo (16) + dirFndrInfo (16) at value offset 22
        dst_img[voff + 22:voff + 54] = src_img[soff + 22:soff + 54]


def build(machfs, base, hd, extras, size, name, desktopdb=True):
    vol = machfs.Volume()
    vol.name = name
    vol.crdate = vol.mddate = base.crdate
    for p in BASE_KEEP:
        put_path(machfs, vol, p, get_path(base, p))
    for special in ('Desktop Folder', 'Trash'):
        vol[special] = machfs.Folder()
    for sub in SYSTEM_SUBFOLDERS:
        if sub not in vol['System Folder']:
            vol['System Folder'][sub] = machfs.Folder()

    for apath, target in ALIASES:
        al = machfs.File()
        al.flags = 0x8000                 # isAlias
        al.aliastarget = get_path(vol, target)
        put_path(machfs, vol, apath, al)

    items = []          # (priority, dest path, node, label)
    if hd is not None:
        for prio, src, dst, *_ in MANIFEST:
            node = None
            for cand in (src, dst):     # dst: source built by this script
                try:
                    node = get_path(hd, cand)
                    break
                except KeyError:
                    pass
            if node is None:
                print(f'  missing in HD image: {":".join(src)}')
                continue
            items.append((prio, dst, node, ':'.join(src)))
    for nm, node in extras:
        items.append((EXTRA_PRIORITY, EXTRA_DEST + (nm,), node, f'extra {nm}'))

    def try_write(sel):
        for prio, dst, node, label in sel:
            put_path(machfs, vol, dst, node)
        try:
            return vol.write(size=size, align=512, desktopdb=desktopdb,
                             bootable=True)
        except (machfs.main.OutOfSpaceError, ValueError):
            return None
        finally:
            for prio, dst, node, label in sel:
                del_path(vol, dst)

    # Drop lowest-priority items until the volume fits, then try to put
    # back dropped items (highest priority first) that fit in the slack.
    dropped = []
    out = try_write(items)
    while out is None:
        if not items:
            raise SystemExit('base system alone does not fit')
        victim = min(items, key=lambda it: it[0])
        items.remove(victim)
        dropped.append(victim)
        out = try_write(items)
    for it in sorted(dropped, key=lambda it: -it[0]):
        o = try_write(items + [it])
        if o is not None:
            items.append(it)
            dropped.remove(it)
            out = o
    for prio, dst, node, label in items:
        put_path(machfs, vol, dst, node)
    dropped = [it[3] for it in dropped]
    return vol, bytearray(out), items, dropped


HOST_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..',
                        'MacintoshESPEmulator', 'host')
# Host prep run: boot the LC II build with the disk writable, let the Finder
# build the desktop database, then Special > Shut Down (cursor starts at
# (15,15); "Special" is at x=232, "Shut Down" 130 px below the menu bar on
# the LC II's 480x320 screen) so the volume is flushed and marked clean.
PREP_ARGS = ['--vclock', '120', '--frames', '80000', '--shot-every', '0',
             '--swipe', '60000:217:-6:2000', '--tap', '62500',
             '--swipe', '63100:0:130:4000']


def prep_desktop(raw_path, out_path, rom_dir):
    """Run the host harness on raw_path; the flushed image goes to out_path."""
    import subprocess
    disk_name = os.path.basename(raw_path)
    build = 'build_diskprep'
    # the image is embedded at link time: drop the old blob object first
    for f in ('rom_blobs.o', 'macemu'):
        try:
            os.remove(os.path.join(HOST_DIR, build, f))
        except FileNotFoundError:
            pass
    r = subprocess.run(['make', '-C', HOST_DIR, f'BUILD={build}', 'MAC_ROM_LC_II=1',
                        f'ROM_DIR={rom_dir}', f'DISK_NAME={disk_name}', '-j8'],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode:
        print(r.stdout[-4000:])
        raise SystemExit('prep: host harness build failed')
    log = out_path + '.prep.log'
    subprocess.run([os.path.join(HOST_DIR, build, 'macemu'), '--disk-rw', out_path,
                    '--log', log] + PREP_ARGS, check=True,
                   stderr=subprocess.DEVNULL)
    img = open(out_path, 'rb').read()
    atrb = struct.unpack_from('>H', img, 1024 + 0x0A)[0]
    if not atrb & 0x0100:
        raise SystemExit(f'prep: volume not cleanly unmounted (see {log})')
    os.remove(log)
    return bytearray(img)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('base', help='bootable System 7.5.3 floppy image')
    ap.add_argument('--hd', help='full System 7.5.3 HD image (manifest source)')
    ap.add_argument('--extra', action='append', default=[],
                    help='MacBinary file, HFS image or folder of them (repeatable)')
    ap.add_argument('--size', default='default',
                    help="'default' (2880K), 'floppy' (1440K), or e.g. 2800K, 2.5M")
    ap.add_argument('--name', default='Macintosh HD', help='volume name (<=27 chars)')
    ap.add_argument('--out', default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), '..', 'ROM', 'custom_753.dsk'))
    ap.add_argument('--exclude', action='append', default=[],
                    help='skip a MANIFEST item by name (repeatable)')
    ap.add_argument('--include', action='append', default=[],
                    help="add an 'optional' MANIFEST item by name (repeatable)")
    ap.add_argument('--prep', action='store_true',
                    help='build the Finder desktop database by booting the image '
                         'in the host harness (LC II build, needs LC-II.ROM next to '
                         '--out, WSL/Linux gcc); without it icons are generic')
    ap.add_argument('--no-desktop', action='store_true',
                    help='omit the Desktop files and clear hasBeenInited, so the '
                         'Finder rebuilds the desktop (host --disk-rw prep step)')
    ap.add_argument('--pylib', help='extra sys.path entry holding machfs/macresources')
    a = ap.parse_args()

    if a.pylib:
        sys.path.insert(0, a.pylib)
    import machfs
    import machfs.main

    size = parse_size(a.size)
    base_img = open(a.base, 'rb').read()
    base = read_volume(machfs, a.base)
    hd = read_volume(machfs, a.hd) if a.hd else None
    extras = list(collect_extras(machfs, a.extra))
    if a.prep:
        a.no_desktop = True
    if a.no_desktop:
        def clear_inited(n):
            for c in n.values():
                if isinstance(c, machfs.File):
                    c.flags &= ~0x0100
                else:
                    clear_inited(c)
        for v in (base, hd):
            if v is not None:
                clear_inited(v)

    global MANIFEST
    MANIFEST = [m for m in MANIFEST if m[2][-1] not in a.exclude and
                ('optional' not in m[3:] or m[2][-1] in a.include)]
    vol, img, items, dropped = build(machfs, base, hd, extras, size, a.name,
                                     desktopdb=not a.no_desktop)

    # Boot blocks: always the base floppy's (machfs takes them from the
    # System's 'boot' 1 resource, which may differ), and bless the System
    # Folder (drFndrInfo[0]) explicitly.
    bp = next(hfs_partitions(base_img))
    if bp[:2] != b'LK':
        raise SystemExit('base image has no boot blocks (LK)')
    img[0:1024] = bp[0:1024]
    # Folder Finder info: copy from the base (Desktop Folder and Trash must
    # exist and be invisible, or the Finder stops with "the desktop folder
    # could not be created" on the read-only volume).
    copy_folder_info(img, bp)
    dirs = catalog_dirs(img)
    for path, rect in FOLDER_WINDOWS.items():
        key = tuple(c.lower() for c in path)
        if key in dirs and dirs[key][1] is not None:
            struct.pack_into('>hhhh', img, dirs[key][1] + 22, *rect)
    sfid = dirs[('system folder',)][0]
    for mdb in (1024, len(img) - 1024):      # MDB and its alternate copy
        struct.pack_into('>L', img, mdb + 0x5C, sfid)

    out = os.path.abspath(a.out)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    if a.prep:
        raw = os.path.join(os.path.dirname(out), '_prep_raw.dsk')
        with open(raw, 'wb') as fh:
            fh.write(img)
        try:
            img = prep_desktop(raw, out, os.path.dirname(out))
        finally:
            os.remove(raw)
        print('  desktop database built by the host harness (clean unmount)')
    else:
        with open(out, 'wb') as fh:
            fh.write(img)

    # Report
    alsz = struct.unpack_from('>L', img, 1024 + 0x14)[0]
    nblk = struct.unpack_from('>H', img, 1024 + 0x12)[0]
    free = struct.unpack_from('>H', img, 1024 + 0x22)[0] * alsz
    print(f'wrote {out}')
    print(f'  image {len(img)} bytes ({len(img)/1024:.0f} K), '
          f'{nblk} alloc blocks of {alsz}, blessed dirID {sfid}')
    kb = lambda n: f'{node_size(machfs, n)/1024:8.1f} K'
    for p in BASE_KEEP:
        print(f'  {kb(get_path(vol, p))}  {":".join(p)}  (base)')
    for prio, dst, node, label in sorted(items, key=lambda it: it[1]):
        print(f'  {kb(node)}  {":".join(dst)}')
    used = node_size(machfs, vol)
    print(f'  files {used/1024:.0f} K, free {free/1024:.0f} K '
          f'({free/1024/1024:.2f} MB)')
    if dropped:
        print('  dropped to fit: ' + ', '.join(dropped))


if __name__ == '__main__':
    main()
