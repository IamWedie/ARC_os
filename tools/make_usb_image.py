#!/usr/bin/env python3
"""Build a bootable ARC OS USB/ISO image (GRUB + kernel, Multiboot2).

Usage:
  python tools/make_usb_image.py            # writes build/arc-os.iso and build/arc-os-usb.img
  python tools/make_usb_image.py --iso-only
  python tools/make_usb_image.py --usb-only

Requirements (any one environment):
  - grub-mkrescue + xorriso (Linux, MSYS2, or WSL)
  - or grub-mkimage + grub-bios-setup/mtools/xorriso

Flashing:
  Windows (Rufus): select arc-os-usb.img/iso, use DD mode for img, MBR/BIOS or
    ISO mode for ISO. Disable Secure Boot and enable CSM/legacy boot on target PC.
  Linux: sudo dd if=build/arc-os-usb.img of=/dev/sdX bs=4M conv=fsync status=progress

The image boots the kernel via Multiboot2 and requests a framebuffer.
"""
import argparse, os, shutil, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OS_ELF = os.path.join(ROOT, "os.elf")
BUILD = os.path.join(ROOT, "build")

GRUB_CFG = """set timeout=0
set default=0

menuentry "ARC OS" {
    insmod multiboot2
    insmod gfxterm
    set gfxpayload=auto
    multiboot2 /boot/os.elf
    boot
}
"""


def run(cmd, **kw):
    print("+ " + " ".join(cmd), flush=True)
    r = subprocess.run(cmd, **kw)
    if r.returncode != 0:
        raise SystemExit("command failed: %s" % cmd[0])


def which(name):
    p = shutil.which(name)
    return p


def build_iso(staging, iso_path):
    mkrescue = which("grub-mkrescue") or which("grub2-mkrescue")
    if mkrescue:
        run([mkrescue, "-o", iso_path, staging])
        return
    grub_mkimage = which("grub-mkimage") or which("grub2-mkimage")
    xorriso = which("xorriso")
    if not grub_mkimage or not xorriso:
        raise SystemExit(
            "Need grub-mkrescue or (grub-mkimage + xorriso). Install GRUB tools.\n"
            "MSYS2: pacman -S grub xorriso mtools\n"
            "Ubuntu: sudo apt install grub-pc-bin grub-common xorriso mtools")
    # Minimal ISO with grub-mkimage core
    iso_dir = staging
    core = os.path.join(staging, "boot", "grub", "i386-pc", "core.img")
    grub_dir = os.path.join(staging, "boot", "grub", "i386-pc")
    os.makedirs(grub_dir, exist_ok=True)
    run([grub_mkimage, "-O", "i386-pc", "-o", core, "-p", "/boot/grub",
         "biosdisk", "part_msdos", "part_gpt", "fat", "normal", "multiboot2", "gfxterm", "video", "echo", "boot"])
    # Create a tiny eltorito image
    elt = os.path.join(grub_dir, "eltorito.img")
    # grub-mkimage -O i386-pc-eltorito produces boot image; fallback to grub-mkrescue requirement
    try:
        run([grub_mkimage, "-O", "i386-pc-eltorito", "-o", elt, "-p", "/boot/grub",
             "biosdisk", "iso9660", "normal", "multiboot2", "gfxterm", "video", "echo", "boot"])
    except SystemExit:
        raise SystemExit("grub-mkrescue required for ISO on this system")
    run([xorriso, "-as", "mkisofs", "-R", "-J", "-b", "boot/grub/i386-pc/eltorito.img",
         "-no-emul-boot", "-boot-load-size", "4", "-boot-info-table", "-o", iso_path, iso_dir])


def build_usb_img(iso_path, img_path):
    # Simple USB image: copy ISO as hybrid if grub-mkrescue produced it, else make FAT image.
    # grub-mkrescue ISOs are usually hybrid-bootable. Copy is the safest baseline.
    shutil.copyfile(iso_path, img_path)
    print("USB image: %s (hybrid ISO copy; dd or Rufus DD mode)" % img_path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iso-only", action="store_true")
    ap.add_argument("--usb-only", action="store_true")
    args = ap.parse_args()
    if not os.path.isfile(OS_ELF):
        raise SystemExit("os.elf not found; run build first")

    os.makedirs(BUILD, exist_ok=True)
    staging = os.path.join(BUILD, "usb_staging")
    if os.path.exists(staging):
        shutil.rmtree(staging)
    os.makedirs(os.path.join(staging, "boot", "grub"), exist_ok=True)
    shutil.copy2(OS_ELF, os.path.join(staging, "boot", "os.elf"))
    with open(os.path.join(staging, "boot", "grub", "grub.cfg"), "w", newline="\n") as f:
        f.write(GRUB_CFG)

    iso_path = os.path.join(BUILD, "arc-os.iso")
    img_path = os.path.join(BUILD, "arc-os-usb.img")
    if not args.usb_only:
        build_iso(staging, iso_path)
        print("ISO: %s" % iso_path)
    if not args.iso_only:
        if not os.path.exists(iso_path):
            build_iso(staging, iso_path)
        build_usb_img(iso_path, img_path)
        print("IMG: %s" % img_path)
    print("Done. Flash with Rufus (DD mode) or dd. Disable Secure Boot/enable CSM on target PC.")


if __name__ == "__main__":
    main()