#!/usr/bin/env python3
"""Create a small MBR/FAT32 raw disk image labelled ARCROOT with welcome.txt.

Output is a raw image usable by QEMU (-drive file=...,format=raw) and by
VirtualBox (convert with VBoxManage convertfromraw, or attach as a raw VMDK).
This tool only writes the requested output file; it never touches real disks.
"""
import argparse
import os
import struct

SECTOR = 512
PART_LBA = 2048
TOTAL_SECTORS = 32768  # 16 MiB
SPC = 1  # sectors per cluster -> 512-byte clusters
RESERVED = 32
NUM_FATS = 2
LABEL = b"ARCROOT    "  # FAT32 11-byte volume label
WELCOME = (
    b"Welcome to ARC OS!\n"
    b"Commands: help, clear, ls, echo, cat <file>, rm <file>\n"
)


def fat_sectors(vol_sectors):
    spf = 1
    while True:
        data = vol_sectors - RESERVED - NUM_FATS * spf
        clusters = data // SPC
        need = (clusters + 2) * 4
        need_spf = (need + SECTOR - 1) // SECTOR
        if need_spf == spf:
            return spf
        spf = need_spf


def make_image(path):
    vol_sectors = TOTAL_SECTORS - PART_LBA
    spf = fat_sectors(vol_sectors)
    img = bytearray(TOTAL_SECTORS * SECTOR)

    # MBR: one FAT32 LBA partition starting at LBA 2048.
    img[446] = 0x80
    img[446 + 4] = 0x0C
    struct.pack_into("<I", img, 446 + 8, PART_LBA)
    struct.pack_into("<I", img, 446 + 12, vol_sectors)
    img[510] = 0x55
    img[511] = 0xAA

    b = PART_LBA * SECTOR
    img[b:b + 3] = b"\xEB\x58\x90"
    img[b + 3:b + 11] = b"MSDOS5.0"
    struct.pack_into("<H", img, b + 11, SECTOR)
    img[b + 13] = SPC
    struct.pack_into("<H", img, b + 14, RESERVED)
    img[b + 16] = NUM_FATS
    struct.pack_into("<H", img, b + 17, 0)
    struct.pack_into("<H", img, b + 19, 0)
    img[b + 21] = 0xF8
    struct.pack_into("<H", img, b + 22, 0)
    struct.pack_into("<H", img, b + 24, 63)
    struct.pack_into("<H", img, b + 26, 255)
    struct.pack_into("<I", img, b + 28, PART_LBA)
    struct.pack_into("<I", img, b + 32, vol_sectors)
    struct.pack_into("<I", img, b + 36, spf)
    struct.pack_into("<H", img, b + 40, 0)
    struct.pack_into("<H", img, b + 42, 0)
    struct.pack_into("<I", img, b + 44, 2)  # root cluster
    struct.pack_into("<H", img, b + 48, 1)  # FSInfo
    struct.pack_into("<H", img, b + 50, 6)  # backup boot
    img[b + 64] = 0x80
    img[b + 66] = 0x29
    struct.pack_into("<I", img, b + 67, 0x12345678)
    img[b + 71:b + 82] = LABEL
    img[b + 82:b + 90] = b"FAT32   "
    img[b + 510] = 0x55
    img[b + 511] = 0xAA
    # Backup boot sector.
    img[b + 6 * SECTOR:b + 7 * SECTOR] = img[b:b + SECTOR]

    # FSInfo.
    fs = b + SECTOR
    struct.pack_into("<I", img, fs + 0, 0x41615252)
    struct.pack_into("<I", img, fs + 484, 0x61417272)
    struct.pack_into("<I", img, fs + 488, 0xFFFFFFFF)
    struct.pack_into("<I", img, fs + 492, 0xFFFFFFFF)
    struct.pack_into("<I", img, fs + 508, 0xAA550000)

    # FAT copies: root cluster 2 and file cluster 3 are end-of-chain.
    fat1 = b + RESERVED * SECTOR
    fat2 = fat1 + spf * SECTOR
    fat_entries = struct.pack(
        "<IIII", 0x0FFFFFF8, 0x0FFFFFFF, 0x0FFFFFFF, 0x0FFFFFFF
    )
    img[fat1:fat1 + len(fat_entries)] = fat_entries
    img[fat2:fat2 + len(fat_entries)] = fat_entries

    # Root directory cluster 2: one 8.3 entry for WELCOME.TXT -> cluster 3.
    cluster_size = SPC * SECTOR
    data_start = RESERVED + NUM_FATS * spf
    root_off = b + (data_start + 0) * cluster_size
    file_off = b + (data_start + 1) * cluster_size
    dirent = bytearray(32)
    dirent[0:11] = b"WELCOME TXT"
    dirent[11] = 0x20  # archive
    struct.pack_into("<H", dirent, 20, 0)       # first cluster high
    struct.pack_into("<H", dirent, 26, 3)       # first cluster low
    struct.pack_into("<I", dirent, 28, len(WELCOME))
    img[root_off:root_off + 32] = dirent
    img[file_off:file_off + len(WELCOME)] = WELCOME

    with open(path, "wb") as f:
        f.write(img)

    print("created %s (%d sectors, %d bytes)" % (path, TOTAL_SECTORS, len(img)))
    print("  MBR partition 1: type=0x0C lba=%d sectors=%d" % (PART_LBA, vol_sectors))
    print("  FAT32 label: ARCROOT  FAT sectors: %d  root cluster: 2" % spf)
    print("  welcome.txt: cluster 3, %d bytes" % len(WELCOME))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("output", help="raw image path to create")
    args = ap.parse_args()
    out = os.path.abspath(args.output)
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    make_image(out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
