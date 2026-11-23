#!/usr/bin/env python3
"""Simple FAT32 consistency checker (M4 fsck-like tool).

Checks: MBR/GPT partition presence, BPB sanity, FAT copy consistency,
and basic directory entry sanity. Read-only; exits non-zero on errors.
"""
import struct, sys, os

SECTOR = 512

def read_sec(f, lba, count=1):
    f.seek(lba * SECTOR)
    data = f.read(count * SECTOR)
    if len(data) != count * SECTOR:
        raise IOError("short read at LBA %d" % lba)
    return data

def check_bpb(bpb, part_lba, part_sectors, errs, warns):
    if bpb[510] != 0x55 or bpb[511] != 0xAA:
        errs.append("missing 0x55AA signature")
    bps = struct.unpack_from("<H", bpb, 11)[0]
    spc = bpb[13]
    reserved = struct.unpack_from("<H", bpb, 14)[0]
    fats = bpb[16]
    spf16 = struct.unpack_from("<H", bpb, 22)[0]
    tot16 = struct.unpack_from("<H", bpb, 19)[0]
    tot32 = struct.unpack_from("<I", bpb, 32)[0]
    spf32 = struct.unpack_from("<I", bpb, 36)[0]
    root_clus = struct.unpack_from("<I", bpb, 44)[0]
    spf = spf16 if spf16 != 0 else spf32
    total = tot16 if tot16 != 0 else tot32
    if bps != 512: errs.append("bytes_per_sector=%d (expected 512)" % bps)
    if spc not in (1,2,4,8,16,32,64,128): errs.append("sectors_per_cluster=%d invalid" % spc)
    if fats < 1 or fats > 2: errs.append("fat_count=%d" % fats)
    if reserved == 0: errs.append("reserved_sectors=0")
    if spf == 0: errs.append("sectors_per_fat=0")
    if root_clus < 2: errs.append("root_cluster=%d" % root_clus)
    if part_sectors and total > part_sectors: errs.append("total_sectors > partition")
    return {"bps":bps,"spc":spc,"reserved":reserved,"fats":fats,"spf":spf,"total":total,"root_clus":root_clus}

def check_fats(f, part_lba, info, errs):
    spf = info["spf"]; fats = info["fats"]; reserved = info["reserved"]
    fat1 = read_sec(f, part_lba + reserved, spf)
    for i in range(1, fats):
        fati = read_sec(f, part_lba + reserved + i*spf, spf)
        if fati != fat1:
            # report first difference
            for j,(a,b) in enumerate(zip(fat1, fati)):
                if a!=b:
                    errs.append("FAT%d differs from FAT1 at byte %d" % (i+1, j))
                    break
    # basic cluster 0/1/root checks
    e0 = struct.unpack_from("<I", fat1, 0)[0] & 0x0FFFFFFF
    e1 = struct.unpack_from("<I", fat1, 4)[0] & 0x0FFFFFFF
    if (e0 & 0x0FFFFFF8) != 0x0FFFFFF8:
        errs.append("FAT[0] media/type bad: 0x%08X" % e0)
    if (e1 & 0x0FFFFFFF) != 0x0FFFFFFF:
        errs.append("FAT[1] bad: 0x%08X" % e1)

def check_root_dir(f, part_lba, info, errs, warns):
    spf = info["spf"]; fats = info["fats"]; reserved = info["reserved"]; spc = info["spc"]
    data_start = reserved + fats*spf
    root = info["root_clus"]
    # walk a limited chain
    fat1 = read_sec(f, part_lba + reserved, spf)
    seen = set()
    clus = root
    entries = 0
    for _ in range(1024):
        if clus < 2 or clus in seen: break
        seen.add(clus)
        lba = part_lba + data_start + (clus-2)*spc
        sec = read_sec(f, lba, spc)
        for off in range(0, len(sec), 32):
            e = sec[off:off+32]
            if e[0] == 0x00: return
            if e[0] == 0xE5: continue
            attr = e[11]
            if attr == 0x0F: continue  # LFN
            name = e[0:11]
            if any(c < 0x20 and c != 0x05 for c in name):
                warns.append("suspicious dirent name bytes at cluster %d off %d" % (clus, off))
            entries += 1
            if entries > 4096:
                warns.append("root dir very large, stopping scan")
                return
        nxt = struct.unpack_from("<I", fat1, clus*4)[0] & 0x0FFFFFFF
        if nxt >= 0x0FFFFFF8: break
        if nxt < 2: errs.append("bad FAT chain entry %d->%d" % (clus,nxt)); break
        clus = nxt

def find_fat32_partitions(f, size):
    parts = []
    try:
        mbr = read_sec(f, 0)
    except Exception as e:
        return parts, ["cannot read MBR: %s" % e]
    if mbr[510] != 0x55 or mbr[511] != 0xAA:
        return parts, []
    # GPT check
    try:
        gpt = read_sec(f, 1)
        if gpt[0:8] == b'EFI PART':
            # minimal GPT scan
            part_lba = struct.unpack_from("<Q", gpt, 72)[0]
            part_count = struct.unpack_from("<I", gpt, 80)[0]
            part_esz = struct.unpack_from("<I", gpt, 84)[0]
            if part_esz < 128: part_esz = 128
            for i in range(min(part_count, 128)):
                off = i * part_esz
                sec_idx = off // 512
                ent = read_sec(f, part_lba + sec_idx)
                e = ent[off % 512: off % 512 + part_esz]
                if len(e) < 128: continue
                type_guid = e[0:16]
                if all(b==0 for b in type_guid): continue
                first = struct.unpack_from("<Q", e, 32)[0]
                last = struct.unpack_from("<Q", e, 40)[0]
                if last >= first:
                    parts.append((first, last-first+1, "gpt"))
            return parts, []
    except Exception:
        pass
    for i in range(4):
        p = mbr[446+i*16:446+(i+1)*16]
        ptype = p[4]
        lba = struct.unpack_from("<I", p, 8)[0]
        cnt = struct.unpack_from("<I", p, 12)[0]
        if ptype in (0x0B,0x0C) and cnt:
            parts.append((lba,cnt,"mbr"))
    return parts, []

def check_image(path):
    errs=[]; warns=[]
    size = os.path.getsize(path)
    with open(path, 'rb') as f:
        parts, perrs = find_fat32_partitions(f, size)
        errs.extend(perrs)
        if not parts:
            # try whole image as volume
            parts = [(0, size//SECTOR, "whole")]
        for lba,cnt,kind in parts:
            print("Checking %s FAT32 volume at LBA %d (%d sectors)" % (kind, lba, cnt))
            try:
                bpb = read_sec(f, lba)
            except Exception as e:
                errs.append("read BPB failed: %s" % e); continue
            info = check_bpb(bpb, lba, cnt, errs, warns)
            if not info: continue
            try: check_fats(f, lba, info, errs)
            except Exception as e: errs.append("FAT check failed: %s" % e)
            try: check_root_dir(f, lba, info, errs, warns)
            except Exception as e: errs.append("root dir check failed: %s" % e)
    for w in warns: print("WARN:", w)
    for e in errs: print("ERROR:", e)
    if errs:
        print("fsck_fat32: FAILED (%d errors, %d warnings)" % (len(errs), len(warns)))
        return 1
    print("fsck_fat32: OK (%d warnings)" % len(warns))
    return 0

if __name__ == '__main__':
    if len(sys.argv) != 2:
        print("usage: fsck_fat32.py <image>")
        sys.exit(2)
    sys.exit(check_image(sys.argv[1]))
