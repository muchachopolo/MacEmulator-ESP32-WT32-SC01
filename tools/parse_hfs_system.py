#!/usr/bin/env python
"""
Parse an HFS disk image to find 'ptch', 'PTCH', 'INIT', and 'boot' resources
in the System file's resource fork.

HFS on-disk structures reference:
  - Inside Macintosh: Files, Chapter 2 (HFS)
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
    """Read a Pascal string (length-prefixed) from data."""
    length = data[offset]
    return data[offset+1:offset+1+length].decode('mac_roman', errors='replace')

def parse_mdb(f):
    """Parse the Master Directory Block at sector 2.

    Inside Macintosh: Files, Table 2-6
    """
    mdb_data = read_at(f, 2 * SECTOR_SIZE, SECTOR_SIZE)

    sig = struct.unpack_from('>H', mdb_data, 0)[0]
    if sig != 0x4244:
        raise ValueError(f"Bad MDB signature: 0x{sig:04X} (expected 0x4244)")

    mdb = {}
    mdb['drSigWord'] = sig
    mdb['drCrDate'] = struct.unpack_from('>I', mdb_data, 2)[0]
    mdb['drLsMod'] = struct.unpack_from('>I', mdb_data, 6)[0]
    mdb['drAtrb'] = struct.unpack_from('>H', mdb_data, 10)[0]
    mdb['drNmFls'] = struct.unpack_from('>H', mdb_data, 12)[0]
    mdb['drVBMSt'] = struct.unpack_from('>H', mdb_data, 14)[0]
    mdb['drAllocPtr'] = struct.unpack_from('>H', mdb_data, 16)[0]
    mdb['drNmAlBlks'] = struct.unpack_from('>H', mdb_data, 18)[0]
    mdb['drAlBlkSiz'] = struct.unpack_from('>I', mdb_data, 20)[0]
    mdb['drClpSiz'] = struct.unpack_from('>I', mdb_data, 24)[0]
    mdb['drAlBlSt'] = struct.unpack_from('>H', mdb_data, 28)[0]
    mdb['drNxtCNID'] = struct.unpack_from('>I', mdb_data, 30)[0]
    mdb['drFreeBks'] = struct.unpack_from('>H', mdb_data, 34)[0]
    mdb['drVN'] = read_pascal_string(mdb_data, 36)

    # Fields after volume name (28 bytes at offset 36)
    mdb['drVolBkUp'] = struct.unpack_from('>I', mdb_data, 64)[0]
    mdb['drVSeqNum'] = struct.unpack_from('>H', mdb_data, 68)[0]
    mdb['drWrCnt'] = struct.unpack_from('>I', mdb_data, 70)[0]
    mdb['drXTClpSiz'] = struct.unpack_from('>I', mdb_data, 74)[0]
    mdb['drCTClpSiz'] = struct.unpack_from('>I', mdb_data, 78)[0]
    mdb['drNmRtDirs'] = struct.unpack_from('>H', mdb_data, 82)[0]
    mdb['drFilCnt'] = struct.unpack_from('>I', mdb_data, 84)[0]
    mdb['drDirCnt'] = struct.unpack_from('>I', mdb_data, 88)[0]
    # drFndrInfo (32 bytes at offset 92) - skip
    # drVCSize (2) at 124, drVBMCSize (2) at 126, drCtlCSize (2) at 128

    # Extent overflow file
    mdb['drXTFlSize'] = struct.unpack_from('>I', mdb_data, 130)[0]
    mdb['drXTExtRec'] = []
    for i in range(3):
        sb = struct.unpack_from('>H', mdb_data, 134 + i*4)[0]
        nb = struct.unpack_from('>H', mdb_data, 136 + i*4)[0]
        mdb['drXTExtRec'].append((sb, nb))

    # Catalog file
    mdb['drCTFlSize'] = struct.unpack_from('>I', mdb_data, 146)[0]
    mdb['drCTExtRec'] = []
    for i in range(3):
        sb = struct.unpack_from('>H', mdb_data, 150 + i*4)[0]
        nb = struct.unpack_from('>H', mdb_data, 152 + i*4)[0]
        mdb['drCTExtRec'].append((sb, nb))

    return mdb

def alloc_block_to_offset(mdb, block):
    """Convert an allocation block number to a byte offset in the disk image."""
    return (mdb['drAlBlSt'] * SECTOR_SIZE) + (block * mdb['drAlBlkSiz'])

def read_extents_data(f, mdb, extents, logical_length):
    """Read data using extent records."""
    data = bytearray()
    for (start_block, num_blocks) in extents:
        if num_blocks == 0:
            break
        offset = alloc_block_to_offset(mdb, start_block)
        size = num_blocks * mdb['drAlBlkSiz']
        data.extend(read_at(f, offset, size))
    return bytes(data[:logical_length])

def parse_btree_header(data):
    """Parse a B-tree header node (node 0)."""
    # Node descriptor (14 bytes)
    nd_flink = struct.unpack_from('>I', data, 0)[0]
    nd_type = data[8]
    nd_nRecs = struct.unpack_from('>H', data, 10)[0]

    # Header record starts at offset 14
    bth = {
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
    return bth

def get_node(btree_data, node_num, node_size):
    """Extract a single B-tree node."""
    offset = node_num * node_size
    return btree_data[offset:offset + node_size]

def get_record_offsets(node_data, node_size):
    """Get the record offsets from the offset table at end of B-tree node."""
    num_recs = struct.unpack_from('>H', node_data, 10)[0]
    offsets = []
    for i in range(num_recs + 1):
        pos = node_size - 2 - (i * 2)
        off = struct.unpack_from('>H', node_data, pos)[0]
        offsets.append(off)
    return offsets, num_recs

def scan_catalog_for_files(catalog_data, btree_hdr):
    """Walk all leaf nodes in the catalog B-tree to find files and directories."""
    node_size = btree_hdr['nodeSize']
    node_num = btree_hdr['firstLeaf']

    system_file_record = None
    all_files = []
    all_dirs = []

    while node_num != 0:
        node_data = get_node(catalog_data, node_num, node_size)
        nd_flink = struct.unpack_from('>I', node_data, 0)[0]
        nd_type = node_data[8]

        if nd_type != 0xFF:  # 0xFF = leaf node
            node_num = nd_flink
            continue

        offsets, num_recs = get_record_offsets(node_data, node_size)

        for i in range(num_recs):
            rec_start = offsets[i]
            if i + 1 < len(offsets):
                rec_end = offsets[i + 1]
            else:
                rec_end = node_size

            if rec_start >= node_size or rec_start < 14:
                continue

            # Catalog key: keyLen(1), resv(1), parentID(4), name(pascal)
            key_len = node_data[rec_start]
            if key_len < 6:
                continue

            parent_id = struct.unpack_from('>I', node_data, rec_start + 2)[0]
            name_len = node_data[rec_start + 6]

            if rec_start + 7 + name_len > len(node_data):
                continue
            name = node_data[rec_start + 7:rec_start + 7 + name_len].decode('mac_roman', errors='replace')

            # Data starts after key, even-aligned
            data_offset = rec_start + 1 + key_len
            if data_offset % 2 != 0:
                data_offset += 1

            if data_offset + 2 > len(node_data):
                continue

            rec_type = struct.unpack_from('>b', node_data, data_offset)[0]

            if rec_type == 1:  # cdrDirRec - directory
                if data_offset + 70 <= len(node_data):
                    dir_cnid = struct.unpack_from('>I', node_data, data_offset + 6)[0]
                    all_dirs.append({
                        'name': name,
                        'parent_id': parent_id,
                        'cnid': dir_cnid,
                    })

            elif rec_type == 2:  # cdrFilRec - file
                # File record: cdrType(1), cdrResrv2(1), filFlags(1), filTyp(1),
                # filUsrWds(16), filFlNum(4), filStBlk(2), filLgLen(4), filPyLen(4),
                # filRStBlk(2), filRLgLen(4), filRPyLen(4), filCrDat(4), filMdDat(4),
                # filBkDat(4), filFndrInfo(16), filClpSiz(2),
                # filExtRec(12), filRExtRec(12), filResrv(4)

                # Offsets relative to data_offset:
                #  0: cdrType (1)  = 2
                #  1: cdrResrv2 (1)
                #  2: filFlags (1)
                #  3: filTyp (1)
                #  4: filUsrWds / FInfo (16 bytes): type(4), creator(4), flags(2), loc(4), folder(2)
                # 20: filFlNum (4) = CNID
                # 24: filStBlk (2) = first alloc block of data fork
                # 26: filLgLen (4) = data fork logical length
                # 30: filPyLen (4) = data fork physical length
                # 34: filRStBlk (2) = first alloc block of rsrc fork
                # 36: filRLgLen (4) = rsrc fork logical length
                # 40: filRPyLen (4) = rsrc fork physical length
                # 44: filCrDat (4)
                # 48: filMdDat (4)
                # 52: filBkDat (4)
                # 56: filFndrInfo / FXInfo (16)
                # 72: filClpSiz (2)
                # 74: filExtRec (12) = 3 data fork extents
                # 86: filRExtRec (12) = 3 rsrc fork extents
                # 98: filResrv (4)

                if data_offset + 98 > len(node_data):
                    continue

                file_type = node_data[data_offset+4:data_offset+8].decode('mac_roman', errors='replace')
                file_creator = node_data[data_offset+8:data_offset+12].decode('mac_roman', errors='replace')
                file_cnid = struct.unpack_from('>I', node_data, data_offset + 20)[0]

                data_fork_lg_len = struct.unpack_from('>I', node_data, data_offset + 26)[0]
                data_fork_py_len = struct.unpack_from('>I', node_data, data_offset + 30)[0]
                rsrc_fork_lg_len = struct.unpack_from('>I', node_data, data_offset + 36)[0]
                rsrc_fork_py_len = struct.unpack_from('>I', node_data, data_offset + 40)[0]

                data_extents = []
                for j in range(3):
                    sb = struct.unpack_from('>H', node_data, data_offset + 74 + j*4)[0]
                    nb = struct.unpack_from('>H', node_data, data_offset + 76 + j*4)[0]
                    data_extents.append((sb, nb))

                rsrc_extents = []
                for j in range(3):
                    sb = struct.unpack_from('>H', node_data, data_offset + 86 + j*4)[0]
                    nb = struct.unpack_from('>H', node_data, data_offset + 88 + j*4)[0]
                    rsrc_extents.append((sb, nb))

                file_info = {
                    'name': name,
                    'parent_id': parent_id,
                    'cnid': file_cnid,
                    'type': file_type,
                    'creator': file_creator,
                    'data_lg_len': data_fork_lg_len,
                    'data_py_len': data_fork_py_len,
                    'rsrc_lg_len': rsrc_fork_lg_len,
                    'rsrc_py_len': rsrc_fork_py_len,
                    'data_extents': data_extents,
                    'rsrc_extents': rsrc_extents,
                }
                all_files.append(file_info)

                if name == "System":
                    system_file_record = file_info

        node_num = nd_flink

    return system_file_record, all_files, all_dirs

def find_overflow_extents(extof_data, extof_hdr, cnid, fork_type):
    """Search extent overflow B-tree for additional extents.

    fork_type: 0x00 = data fork, 0xFF = resource fork
    """
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
            rec_start_block = struct.unpack_from('>H', node_data, rec_start + 6)[0]

            if rec_file_id == cnid and rec_fork_type == fork_type:
                d_off = rec_start + 1 + key_len
                if d_off % 2 != 0:
                    d_off += 1

                print(f"    Overflow extent for CNID {cnid} fork=0x{fork_type:02X} startBlock={rec_start_block}:")
                for j in range(3):
                    if d_off + j*4 + 4 <= node_size:
                        sb = struct.unpack_from('>H', node_data, d_off + j*4)[0]
                        nb = struct.unpack_from('>H', node_data, d_off + j*4 + 2)[0]
                        if nb > 0:
                            additional_extents.append((sb, nb))
                            print(f"      extent: start={sb}, count={nb}")

        node_num = nd_flink

    return additional_extents

def parse_resource_fork(rsrc_data):
    """Parse a Macintosh resource fork and return all resources."""
    if len(rsrc_data) < 16:
        print("ERROR: Resource fork too small")
        return []

    data_offset = struct.unpack_from('>I', rsrc_data, 0)[0]
    map_offset = struct.unpack_from('>I', rsrc_data, 4)[0]
    data_length = struct.unpack_from('>I', rsrc_data, 8)[0]
    map_length = struct.unpack_from('>I', rsrc_data, 12)[0]

    print(f"\n  Resource fork header:")
    print(f"    Data offset:  {data_offset} (0x{data_offset:X})")
    print(f"    Map offset:   {map_offset} (0x{map_offset:X})")
    print(f"    Data length:  {data_length}")
    print(f"    Map length:   {map_length}")
    print(f"    Rsrc data sz: {len(rsrc_data)}")

    if map_offset + map_length > len(rsrc_data):
        print(f"  WARNING: Map extends beyond resource data! (need {map_offset+map_length}, have {len(rsrc_data)})")

    rsrc_map = rsrc_data[map_offset:]

    if len(rsrc_map) < 28:
        print("ERROR: Resource map too small")
        return []

    res_file_attrs = struct.unpack_from('>H', rsrc_map, 22)[0]
    type_list_offset = struct.unpack_from('>H', rsrc_map, 24)[0]
    name_list_offset = struct.unpack_from('>H', rsrc_map, 26)[0]

    print(f"    File attrs:    0x{res_file_attrs:04X}")
    print(f"    Type list off: {type_list_offset} (from map start)")
    print(f"    Name list off: {name_list_offset} (from map start)")

    type_list_start = type_list_offset
    if type_list_start + 2 > len(rsrc_map):
        print("ERROR: Type list offset out of range")
        return []

    num_types_minus1 = struct.unpack_from('>h', rsrc_map, type_list_start)[0]
    num_types = num_types_minus1 + 1
    print(f"\n  Number of resource types: {num_types}")

    all_resources = []

    for t in range(num_types):
        type_entry_offset = type_list_start + 2 + (t * 8)
        if type_entry_offset + 8 > len(rsrc_map):
            break

        res_type_bytes = rsrc_map[type_entry_offset:type_entry_offset + 4]
        res_type = res_type_bytes.decode('mac_roman', errors='replace')
        num_res_minus1 = struct.unpack_from('>H', rsrc_map, type_entry_offset + 4)[0]
        num_res = num_res_minus1 + 1
        ref_list_offset = struct.unpack_from('>H', rsrc_map, type_entry_offset + 6)[0]

        resources_of_type = []

        for r in range(num_res):
            ref_offset = type_list_start + ref_list_offset + (r * 12)
            if ref_offset + 12 > len(rsrc_map):
                break

            res_id = struct.unpack_from('>h', rsrc_map, ref_offset)[0]
            name_off = struct.unpack_from('>H', rsrc_map, ref_offset + 2)[0]
            res_attrs = rsrc_map[ref_offset + 4]

            # 3-byte data offset from start of resource data area
            d_off_bytes = rsrc_map[ref_offset + 5:ref_offset + 8]
            res_data_off = (d_off_bytes[0] << 16) | (d_off_bytes[1] << 8) | d_off_bytes[2]

            # Get resource name
            res_name = None
            if name_off != 0xFFFF:
                name_abs = name_list_offset + name_off
                if name_abs < len(rsrc_map):
                    nlen = rsrc_map[name_abs]
                    if name_abs + 1 + nlen <= len(rsrc_map):
                        res_name = rsrc_map[name_abs + 1:name_abs + 1 + nlen].decode('mac_roman', errors='replace')

            # Get resource data size (4-byte length prefix in data area)
            abs_data_off = data_offset + res_data_off
            res_size = 0
            if abs_data_off + 4 <= len(rsrc_data):
                res_size = struct.unpack_from('>I', rsrc_data, abs_data_off)[0]

            resources_of_type.append({
                'type': res_type,
                'id': res_id,
                'name': res_name,
                'attrs': res_attrs,
                'data_offset': res_data_off,
                'size': res_size,
            })

        all_resources.append((res_type, resources_of_type))

    return all_resources


def main():
    print(f"{'='*60}")
    print(f"  HFS Disk Image Analyzer")
    print(f"  File: {DISK_PATH}")
    print(f"  Size: {os.path.getsize(DISK_PATH)} bytes")
    print(f"{'='*60}")
    print()

    with open(DISK_PATH, 'rb') as f:
        # === Step 1: Parse MDB ===
        print("--- Master Directory Block (MDB) ---")
        mdb = parse_mdb(f)
        print(f"  Volume name:        '{mdb['drVN']}'")
        print(f"  Files in root:      {mdb['drNmFls']}")
        print(f"  Dirs in root:       {mdb['drNmRtDirs']}")
        print(f"  Total files:        {mdb['drFilCnt']}")
        print(f"  Total dirs:         {mdb['drDirCnt']}")
        print(f"  Alloc block size:   {mdb['drAlBlkSiz']} bytes")
        print(f"  Total alloc blocks: {mdb['drNmAlBlks']}")
        print(f"  Free alloc blocks:  {mdb['drFreeBks']}")
        print(f"  First AB at sector: {mdb['drAlBlSt']}")
        print(f"  Catalog file size:  {mdb['drCTFlSize']} bytes")
        print(f"  Catalog extents:    {mdb['drCTExtRec']}")
        print(f"  Ext OF file size:   {mdb['drXTFlSize']} bytes")
        print(f"  Ext OF extents:     {mdb['drXTExtRec']}")
        print(f"  Next CNID:          {mdb['drNxtCNID']}")
        print()

        # === Step 2: Read catalog B-tree ===
        print("--- Catalog B-Tree ---")
        catalog_data = read_extents_data(f, mdb, mdb['drCTExtRec'], mdb['drCTFlSize'])
        print(f"  Read {len(catalog_data)} bytes")

        btree_hdr = parse_btree_header(catalog_data)
        print(f"  Depth:      {btree_hdr['depth']}")
        print(f"  Root node:  {btree_hdr['rootNode']}")
        print(f"  Records:    {btree_hdr['numRecords']}")
        print(f"  First leaf: {btree_hdr['firstLeaf']}")
        print(f"  Last leaf:  {btree_hdr['lastLeaf']}")
        print(f"  Node size:  {btree_hdr['nodeSize']} bytes")
        print()

        # === Step 3: Scan catalog ===
        print("--- Catalog Scan ---")
        system_file, all_files, all_dirs = scan_catalog_for_files(catalog_data, btree_hdr)

        # Print directory listing
        print(f"\n  Directories ({len(all_dirs)}):")
        for d in all_dirs:
            print(f"    CNID={d['cnid']:4d}  parent={d['parent_id']:4d}  '{d['name']}'")

        print(f"\n  Files ({len(all_files)}):")
        for fi in sorted(all_files, key=lambda x: (x['parent_id'], x['name'])):
            rsrc_info = f"  rsrc={fi['rsrc_lg_len']:>8d}" if fi['rsrc_lg_len'] > 0 else ""
            print(f"    CNID={fi['cnid']:4d}  parent={fi['parent_id']:4d}  "
                  f"'{fi['name']}'  type='{fi['type']}' creator='{fi['creator']}'  "
                  f"data={fi['data_lg_len']:>8d}{rsrc_info}")

        if system_file is None:
            print("\n  *** System file not found! ***")
            for fi in all_files:
                if 'system' in fi['name'].lower():
                    print(f"    Possible match: '{fi['name']}' (CNID={fi['cnid']})")
            return

        print(f"\n--- System File Details ---")
        print(f"  Name:          '{system_file['name']}'")
        print(f"  CNID:          {system_file['cnid']}")
        print(f"  Parent CNID:   {system_file['parent_id']}")
        print(f"  Type:          '{system_file['type']}'")
        print(f"  Creator:       '{system_file['creator']}'")
        print(f"  Data fork:     {system_file['data_lg_len']} bytes (phys {system_file['data_py_len']})")
        print(f"  Resource fork: {system_file['rsrc_lg_len']} bytes (phys {system_file['rsrc_py_len']})")
        print(f"  Data extents:  {system_file['data_extents']}")
        print(f"  Rsrc extents:  {system_file['rsrc_extents']}")

        if system_file['rsrc_lg_len'] == 0:
            print("  System file has no resource fork!")
            return

        # === Step 4: Read resource fork ===
        print(f"\n--- Reading System Resource Fork ---")

        # Check if 3 extents are enough
        covered = sum(nb * mdb['drAlBlkSiz'] for _, nb in system_file['rsrc_extents'] if nb > 0)
        needed = system_file['rsrc_lg_len']
        print(f"  Inline extents cover: {covered} bytes")
        print(f"  Need:                 {needed} bytes")

        all_rsrc_extents = list(system_file['rsrc_extents'])

        if covered < needed:
            print(f"  Need overflow extents...")
            # Read extent overflow file
            if mdb['drXTFlSize'] > 0:
                extof_data = read_extents_data(f, mdb, mdb['drXTExtRec'], mdb['drXTFlSize'])
                extof_hdr = parse_btree_header(extof_data)
                print(f"  Extent overflow B-tree: {extof_hdr['numRecords']} records")

                overflow_extents = find_overflow_extents(
                    extof_data, extof_hdr, system_file['cnid'], 0xFF)

                if overflow_extents:
                    all_rsrc_extents.extend(overflow_extents)
                    covered2 = sum(nb * mdb['drAlBlkSiz'] for _, nb in all_rsrc_extents if nb > 0)
                    print(f"  After overflow: {covered2} bytes covered")

        rsrc_data = read_extents_data(f, mdb, all_rsrc_extents, system_file['rsrc_lg_len'])
        print(f"  Read {len(rsrc_data)} bytes of resource fork")

        if len(rsrc_data) < system_file['rsrc_lg_len']:
            print(f"  WARNING: Short read! Got {len(rsrc_data)} of {system_file['rsrc_lg_len']}")

        # === Step 5: Parse resource fork ===
        print(f"\n--- Parsing Resource Fork ---")
        all_resources = parse_resource_fork(rsrc_data)

        # Summary of all types
        print(f"\n  {'='*55}")
        print(f"  Resource Type Summary ({sum(len(r) for _, r in all_resources)} total resources)")
        print(f"  {'='*55}")
        for res_type, resources in sorted(all_resources, key=lambda x: x[0]):
            type_hex = ''.join(f'{b:02X}' for b in res_type.encode('mac_roman'))
            total_size = sum(r['size'] for r in resources)
            print(f"  '{res_type}' (0x{type_hex}): {len(resources):3d} resource(s), "
                  f"total {total_size:>8d} bytes")

        # Detailed listings for types of interest
        target_types = {'ptch', 'PTCH', 'INIT', 'boot', 'PACK', 'DRVR', 'WDEF', 'MDEF',
                        'CDEF', 'lmgr', 'lpch', 'ROv#', 'gbly', 'scod'}

        print(f"\n  {'='*55}")
        print(f"  Detailed Listings for Key Resource Types")
        print(f"  {'='*55}")

        for res_type, resources in all_resources:
            if res_type.strip() in target_types or res_type in target_types:
                print(f"\n  --- '{res_type}' ({len(resources)} resources) ---")
                for res in sorted(resources, key=lambda x: x['id']):
                    name_str = f"  name='{res['name']}'" if res['name'] else ""
                    print(f"    ID={res['id']:6d}  size={res['size']:8d}  "
                          f"attrs=0x{res['attrs']:02X}{name_str}")

        # Specific summary for ptch/PTCH
        print(f"\n  {'='*55}")
        print(f"  'ptch' and 'PTCH' Resources")
        print(f"  {'='*55}")
        found_ptch = False
        for res_type, resources in all_resources:
            if res_type.strip() in ('ptch', 'PTCH'):
                found_ptch = True
                print(f"\n  Type '{res_type}': {len(resources)} resources")
                for res in sorted(resources, key=lambda x: x['id']):
                    name_str = f"  name='{res['name']}'" if res['name'] else ""
                    print(f"    ID={res['id']:6d}  size={res['size']:8d}  "
                          f"attrs=0x{res['attrs']:02X}{name_str}")
        if not found_ptch:
            print("  No 'ptch' or 'PTCH' resources found.")

        # boot / INIT summary
        print(f"\n  {'='*55}")
        print(f"  'boot' and 'INIT' Resources")
        print(f"  {'='*55}")
        found_boot = False
        for res_type, resources in all_resources:
            if res_type.strip() in ('boot', 'INIT'):
                found_boot = True
                print(f"\n  Type '{res_type}': {len(resources)} resources")
                for res in sorted(resources, key=lambda x: x['id']):
                    name_str = f"  name='{res['name']}'" if res['name'] else ""
                    print(f"    ID={res['id']:6d}  size={res['size']:8d}  "
                          f"attrs=0x{res['attrs']:02X}{name_str}")
        if not found_boot:
            print("  No 'boot' or 'INIT' resources found.")

        print(f"\n{'='*60}")
        print(f"  Analysis complete.")
        print(f"{'='*60}")


if __name__ == '__main__':
    main()
