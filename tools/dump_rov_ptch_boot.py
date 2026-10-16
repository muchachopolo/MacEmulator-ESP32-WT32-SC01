#!/usr/bin/env python
"""
Dump hex data for ROv# 630, PTCH 630, and boot resources from the System
file's resource fork in the Mac HFS disk image.

Reuses parsing logic from parse_hfs_system.py.
"""

import struct
import sys
import os

SECTOR_SIZE = 512
DISK_PATH = r"D:\Personal\Macintosh\ROM\OS_750_boot.dsk"


def read_at(f, offset, size):
    f.seek(offset)
    data = f.read(size)
    if len(data) < size:
        raise IOError(f"Short read at offset {offset}: wanted {size}, got {len(data)}")
    return data


def read_pascal_string(data, offset):
    length = data[offset]
    return data[offset+1:offset+1+length].decode('mac_roman', errors='replace')


def parse_mdb(f):
    mdb_data = read_at(f, 2 * SECTOR_SIZE, SECTOR_SIZE)
    sig = struct.unpack_from('>H', mdb_data, 0)[0]
    if sig != 0x4244:
        raise ValueError(f"Bad MDB signature: 0x{sig:04X}")
    mdb = {}
    mdb['drNmAlBlks'] = struct.unpack_from('>H', mdb_data, 18)[0]
    mdb['drAlBlkSiz'] = struct.unpack_from('>I', mdb_data, 20)[0]
    mdb['drAlBlSt'] = struct.unpack_from('>H', mdb_data, 28)[0]
    mdb['drCTFlSize'] = struct.unpack_from('>I', mdb_data, 146)[0]
    mdb['drCTExtRec'] = []
    for i in range(3):
        sb = struct.unpack_from('>H', mdb_data, 150 + i*4)[0]
        nb = struct.unpack_from('>H', mdb_data, 152 + i*4)[0]
        mdb['drCTExtRec'].append((sb, nb))
    mdb['drXTFlSize'] = struct.unpack_from('>I', mdb_data, 130)[0]
    mdb['drXTExtRec'] = []
    for i in range(3):
        sb = struct.unpack_from('>H', mdb_data, 134 + i*4)[0]
        nb = struct.unpack_from('>H', mdb_data, 136 + i*4)[0]
        mdb['drXTExtRec'].append((sb, nb))
    return mdb


def alloc_block_to_offset(mdb, block):
    return (mdb['drAlBlSt'] * SECTOR_SIZE) + (block * mdb['drAlBlkSiz'])


def read_extents_data(f, mdb, extents, logical_length):
    data = bytearray()
    for (start_block, num_blocks) in extents:
        if num_blocks == 0:
            break
        offset = alloc_block_to_offset(mdb, start_block)
        size = num_blocks * mdb['drAlBlkSiz']
        data.extend(read_at(f, offset, size))
    return bytes(data[:logical_length])


def parse_btree_header(data):
    return {
        'depth': struct.unpack_from('>H', data, 14)[0],
        'rootNode': struct.unpack_from('>I', data, 16)[0],
        'numRecords': struct.unpack_from('>I', data, 20)[0],
        'firstLeaf': struct.unpack_from('>I', data, 24)[0],
        'lastLeaf': struct.unpack_from('>I', data, 28)[0],
        'nodeSize': struct.unpack_from('>H', data, 32)[0],
        'maxKeyLen': struct.unpack_from('>H', data, 34)[0],
        'numNodes': struct.unpack_from('>I', data, 36)[0],
        'freeNodes': struct.unpack_from('>I', data, 40)[0],
    }


def get_node(btree_data, node_num, node_size):
    offset = node_num * node_size
    return btree_data[offset:offset + node_size]


def get_record_offsets(node_data, node_size):
    num_recs = struct.unpack_from('>H', node_data, 10)[0]
    offsets = []
    for i in range(num_recs + 1):
        pos = node_size - 2 - (i * 2)
        off = struct.unpack_from('>H', node_data, pos)[0]
        offsets.append(off)
    return offsets, num_recs


def scan_catalog_for_system(catalog_data, btree_hdr):
    """Walk catalog B-tree to find the System file record."""
    node_size = btree_hdr['nodeSize']
    node_num = btree_hdr['firstLeaf']

    while node_num != 0:
        node_data = get_node(catalog_data, node_num, node_size)
        nd_flink = struct.unpack_from('>I', node_data, 0)[0]
        nd_type = node_data[8]

        if nd_type != 0xFF:
            node_num = nd_flink
            continue

        offsets, num_recs = get_record_offsets(node_data, node_size)

        for i in range(num_recs):
            rec_start = offsets[i]
            if rec_start >= node_size or rec_start < 14:
                continue

            key_len = node_data[rec_start]
            if key_len < 6:
                continue

            parent_id = struct.unpack_from('>I', node_data, rec_start + 2)[0]
            name_len = node_data[rec_start + 6]
            if rec_start + 7 + name_len > len(node_data):
                continue
            name = node_data[rec_start + 7:rec_start + 7 + name_len].decode('mac_roman', errors='replace')

            data_offset = rec_start + 1 + key_len
            if data_offset % 2 != 0:
                data_offset += 1
            if data_offset + 2 > len(node_data):
                continue

            rec_type = struct.unpack_from('>b', node_data, data_offset)[0]

            if rec_type == 2 and name == "System":
                if data_offset + 98 > len(node_data):
                    continue

                rsrc_fork_lg_len = struct.unpack_from('>I', node_data, data_offset + 36)[0]
                file_cnid = struct.unpack_from('>I', node_data, data_offset + 20)[0]

                rsrc_extents = []
                for j in range(3):
                    sb = struct.unpack_from('>H', node_data, data_offset + 86 + j*4)[0]
                    nb = struct.unpack_from('>H', node_data, data_offset + 88 + j*4)[0]
                    rsrc_extents.append((sb, nb))

                return {
                    'cnid': file_cnid,
                    'rsrc_lg_len': rsrc_fork_lg_len,
                    'rsrc_extents': rsrc_extents,
                }

        node_num = nd_flink

    return None


def find_overflow_extents(extof_data, extof_hdr, cnid, fork_type):
    node_size = extof_hdr['nodeSize']
    node_num = extof_hdr['firstLeaf']
    additional_extents = []

    while node_num != 0:
        node_data = get_node(extof_data, node_num, node_size)
        nd_flink = struct.unpack_from('>I', node_data, 0)[0]
        nd_type = node_data[8]

        if nd_type != 0xFF:
            node_num = nd_flink
            continue

        offsets, num_recs = get_record_offsets(node_data, node_size)

        for i in range(num_recs):
            rec_start = offsets[i]
            if rec_start + 8 > node_size:
                continue

            key_len = node_data[rec_start]
            if key_len < 7:
                continue

            rec_fork_type = node_data[rec_start + 1]
            rec_file_id = struct.unpack_from('>I', node_data, rec_start + 2)[0]

            if rec_file_id == cnid and rec_fork_type == fork_type:
                d_off = rec_start + 1 + key_len
                if d_off % 2 != 0:
                    d_off += 1
                for j in range(3):
                    if d_off + j*4 + 4 <= node_size:
                        sb = struct.unpack_from('>H', node_data, d_off + j*4)[0]
                        nb = struct.unpack_from('>H', node_data, d_off + j*4 + 2)[0]
                        if nb > 0:
                            additional_extents.append((sb, nb))

        node_num = nd_flink

    return additional_extents


def parse_resource_fork_full(rsrc_data):
    """Parse resource fork, returning resources keyed by (type, id) with actual data bytes."""
    if len(rsrc_data) < 16:
        raise ValueError("Resource fork too small")

    data_offset = struct.unpack_from('>I', rsrc_data, 0)[0]
    map_offset = struct.unpack_from('>I', rsrc_data, 4)[0]
    data_length = struct.unpack_from('>I', rsrc_data, 8)[0]
    map_length = struct.unpack_from('>I', rsrc_data, 12)[0]

    rsrc_map = rsrc_data[map_offset:]
    if len(rsrc_map) < 28:
        raise ValueError("Resource map too small")

    type_list_offset = struct.unpack_from('>H', rsrc_map, 24)[0]
    name_list_offset = struct.unpack_from('>H', rsrc_map, 26)[0]

    type_list_start = type_list_offset
    num_types_minus1 = struct.unpack_from('>h', rsrc_map, type_list_start)[0]
    num_types = num_types_minus1 + 1

    resources = {}  # (type_str, id) -> { 'data': bytes, 'name': str|None, ... }

    for t in range(num_types):
        type_entry_offset = type_list_start + 2 + (t * 8)
        if type_entry_offset + 8 > len(rsrc_map):
            break

        res_type_bytes = rsrc_map[type_entry_offset:type_entry_offset + 4]
        res_type = res_type_bytes.decode('mac_roman', errors='replace')
        num_res_minus1 = struct.unpack_from('>H', rsrc_map, type_entry_offset + 4)[0]
        num_res = num_res_minus1 + 1
        ref_list_offset = struct.unpack_from('>H', rsrc_map, type_entry_offset + 6)[0]

        for r in range(num_res):
            ref_offset = type_list_start + ref_list_offset + (r * 12)
            if ref_offset + 12 > len(rsrc_map):
                break

            res_id = struct.unpack_from('>h', rsrc_map, ref_offset)[0]
            name_off = struct.unpack_from('>H', rsrc_map, ref_offset + 2)[0]
            res_attrs = rsrc_map[ref_offset + 4]

            d_off_bytes = rsrc_map[ref_offset + 5:ref_offset + 8]
            res_data_off = (d_off_bytes[0] << 16) | (d_off_bytes[1] << 8) | d_off_bytes[2]

            res_name = None
            if name_off != 0xFFFF:
                name_abs = name_list_offset + name_off
                if name_abs < len(rsrc_map):
                    nlen = rsrc_map[name_abs]
                    if name_abs + 1 + nlen <= len(rsrc_map):
                        res_name = rsrc_map[name_abs + 1:name_abs + 1 + nlen].decode('mac_roman', errors='replace')

            abs_data_off = data_offset + res_data_off
            if abs_data_off + 4 <= len(rsrc_data):
                res_size = struct.unpack_from('>I', rsrc_data, abs_data_off)[0]
                res_bytes = rsrc_data[abs_data_off + 4:abs_data_off + 4 + res_size]
            else:
                res_size = 0
                res_bytes = b''

            resources[(res_type, res_id)] = {
                'data': res_bytes,
                'size': res_size,
                'name': res_name,
                'attrs': res_attrs,
            }

    return resources


def hex_dump(data, max_bytes=None, prefix="    "):
    """Pretty hex dump with ASCII sidebar."""
    if max_bytes is not None:
        data = data[:max_bytes]
    lines = []
    for i in range(0, len(data), 16):
        chunk = data[i:i+16]
        hex_part = ' '.join(f'{b:02X}' for b in chunk)
        ascii_part = ''.join(chr(b) if 32 <= b < 127 else '.' for b in chunk)
        lines.append(f"{prefix}{i:04X}: {hex_part:<48s}  {ascii_part}")
    return '\n'.join(lines)


def main():
    print(f"{'='*70}")
    print(f"  ROv#, PTCH, and boot Resource Dumper")
    print(f"  Disk: {DISK_PATH}")
    print(f"{'='*70}")

    with open(DISK_PATH, 'rb') as f:
        # Parse MDB
        mdb = parse_mdb(f)
        print(f"\n  Alloc block size: {mdb['drAlBlkSiz']} bytes")
        print(f"  First AB sector:  {mdb['drAlBlSt']}")

        # Read catalog B-tree
        catalog_data = read_extents_data(f, mdb, mdb['drCTExtRec'], mdb['drCTFlSize'])
        btree_hdr = parse_btree_header(catalog_data)

        # Find System file
        sys_file = scan_catalog_for_system(catalog_data, btree_hdr)
        if sys_file is None:
            print("  ERROR: System file not found!")
            return

        print(f"  System file CNID: {sys_file['cnid']}")
        print(f"  Resource fork:    {sys_file['rsrc_lg_len']} bytes")

        # Collect all resource fork extents (inline + overflow)
        all_rsrc_extents = list(sys_file['rsrc_extents'])
        covered = sum(nb * mdb['drAlBlkSiz'] for _, nb in all_rsrc_extents if nb > 0)
        needed = sys_file['rsrc_lg_len']

        if covered < needed and mdb['drXTFlSize'] > 0:
            extof_data = read_extents_data(f, mdb, mdb['drXTExtRec'], mdb['drXTFlSize'])
            extof_hdr = parse_btree_header(extof_data)
            overflow = find_overflow_extents(extof_data, extof_hdr, sys_file['cnid'], 0xFF)
            all_rsrc_extents.extend(overflow)

        rsrc_data = read_extents_data(f, mdb, all_rsrc_extents, sys_file['rsrc_lg_len'])
        print(f"  Resource fork read: {len(rsrc_data)} bytes")

        # Parse all resources
        resources = parse_resource_fork_full(rsrc_data)

        # ===== 1. List ALL ROv# resources =====
        print(f"\n{'='*70}")
        print(f"  ALL ROv# Resources")
        print(f"{'='*70}")
        rov_count = 0
        for (rtype, rid), res in sorted(resources.items()):
            if rtype == 'ROv#':
                rov_count += 1
                name_str = f"  name='{res['name']}'" if res['name'] else ""
                print(f"\n  ROv# ID={rid}  size={res['size']} bytes  attrs=0x{res['attrs']:02X}{name_str}")
                print(hex_dump(res['data']))
        if rov_count == 0:
            print("  No ROv# resources found!")

        # ===== 2. Dump ROv# 630 specifically =====
        print(f"\n{'='*70}")
        print(f"  ROv# ID=630 (Mac Classic ROM version)")
        print(f"{'='*70}")
        key_rov = ('ROv#', 630)
        if key_rov in resources:
            res = resources[key_rov]
            data = res['data']
            print(f"  Size: {res['size']} bytes")
            print(f"  Full hex dump:")
            print(hex_dump(data))

            # Try to interpret as a list of resource IDs
            # Common formats: count (2 bytes) followed by 2-byte IDs
            if len(data) >= 2:
                print(f"\n  Interpretation attempts:")
                # Try: first word = count, then list of word-sized IDs
                count_16 = struct.unpack_from('>H', data, 0)[0]
                print(f"    First word as count (big-endian uint16): {count_16}")
                if count_16 > 0 and 2 + count_16 * 2 <= len(data):
                    ids = []
                    for i in range(count_16):
                        rid = struct.unpack_from('>h', data, 2 + i * 2)[0]
                        ids.append(rid)
                    print(f"    IDs (signed int16): {ids}")
                elif count_16 > 0:
                    print(f"    (count {count_16} doesn't fit in {len(data)} bytes as 2-byte IDs)")

                # Try: first word = count, then list of long-sized IDs
                if count_16 > 0 and 2 + count_16 * 4 <= len(data):
                    ids = []
                    for i in range(count_16):
                        rid = struct.unpack_from('>i', data, 2 + i * 4)[0]
                        ids.append(rid)
                    print(f"    IDs (signed int32): {ids}")

                # Try: no count, all 2-byte signed IDs
                if len(data) % 2 == 0:
                    all_ids = []
                    for i in range(0, len(data), 2):
                        rid = struct.unpack_from('>h', data, i)[0]
                        all_ids.append(rid)
                    print(f"    All words as int16: {all_ids}")

                # Try: first byte = count, then 2-byte IDs
                count_8 = data[0]
                if count_8 > 0 and 1 + count_8 * 2 <= len(data):
                    ids = []
                    for i in range(count_8):
                        rid = struct.unpack_from('>h', data, 1 + i * 2)[0]
                        ids.append(rid)
                    print(f"    First byte as count ({count_8}), then int16 IDs: {ids}")

        else:
            print("  NOT FOUND!")
            # Show what ROv# IDs exist
            existing = [rid for (rtype, rid) in resources if rtype == 'ROv#']
            print(f"  Available ROv# IDs: {sorted(existing)}")

        # ===== 3. Dump first 64 bytes of PTCH 630 =====
        print(f"\n{'='*70}")
        print(f"  PTCH ID=630 (first 64 bytes)")
        print(f"{'='*70}")
        # Check both 'ptch' and 'PTCH'
        for type_name in ['PTCH', 'ptch']:
            key_ptch = (type_name, 630)
            if key_ptch in resources:
                res = resources[key_ptch]
                print(f"  Type: '{type_name}'  Size: {res['size']} bytes")
                name_str = f"  Name: '{res['name']}'" if res['name'] else ""
                if name_str:
                    print(f"  {name_str}")
                print(f"  First 64 bytes:")
                print(hex_dump(res['data'], max_bytes=64))
                break
        else:
            print("  NOT FOUND (neither 'PTCH' nor 'ptch' with ID 630)")
            # Show what ptch/PTCH IDs exist
            for type_name in ['PTCH', 'ptch']:
                existing = sorted([rid for (rtype, rid) in resources if rtype == type_name])
                if existing:
                    print(f"  Available '{type_name}' IDs: {existing}")

        # ===== 4. Dump first 16 bytes of each boot resource =====
        print(f"\n{'='*70}")
        print(f"  'boot' Resources (first 16 bytes each)")
        print(f"{'='*70}")
        boot_resources = sorted([(rid, res) for (rtype, rid), res in resources.items() if rtype == 'boot'],
                                key=lambda x: x[0])
        if boot_resources:
            for rid, res in boot_resources:
                name_str = f"  name='{res['name']}'" if res['name'] else ""
                print(f"\n  boot ID={rid}  size={res['size']} bytes  attrs=0x{res['attrs']:02X}{name_str}")
                print(hex_dump(res['data'], max_bytes=16))
        else:
            print("  No 'boot' resources found!")

        # ===== 5. Also show what ptch/PTCH resources exist for cross-reference =====
        print(f"\n{'='*70}")
        print(f"  All ptch/PTCH Resources (for cross-reference)")
        print(f"{'='*70}")
        for type_name in ['ptch', 'PTCH']:
            ptch_resources = sorted([(rid, res) for (rtype, rid), res in resources.items() if rtype == type_name],
                                    key=lambda x: x[0])
            if ptch_resources:
                print(f"\n  Type '{type_name}': {len(ptch_resources)} resources")
                for rid, res in ptch_resources:
                    name_str = f"  name='{res['name']}'" if res['name'] else ""
                    print(f"    ID={rid:6d}  size={res['size']:8d}  attrs=0x{res['attrs']:02X}{name_str}")

    print(f"\n{'='*70}")
    print(f"  Done.")
    print(f"{'='*70}")


if __name__ == '__main__':
    main()
