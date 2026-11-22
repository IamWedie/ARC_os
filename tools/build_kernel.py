#!/usr/bin/env python3
"""
ARC OS full build: kernel + embedded ring-3 shell + multiboot boot image.

Produces os.elf, a 32-bit multiboot ELF that QEMU/GRUB can load directly.
Every stage fails loudly; stale outputs are deleted before the build starts
so a failed build can never leave a runnable-looking image behind.

Usage:
    python tools/build_kernel.py
    python tools/build_kernel.py --keep-build   # reuse objects in BUILD

Environment overrides:
    ZIG        path to zig.exe
    OBJCOPY    path to objcopy.exe
    ARC_BUILD  build directory (default: <temp>/opencode/arc_build)
"""
import glob
import os
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.environ.get(
    "ARC_BUILD",
    os.path.join(tempfile.gettempdir(), "opencode", "arc_build"),
)

MULTIBOOT_MAGIC = 0x1BADB002
KERNEL_LOAD = 0x200000
QUIET = False


class BuildError(RuntimeError):
    pass


def log(msg):
    print(msg, flush=True)


def verify_user_hello_blob():
    """The boot-time program registry needs a nonempty second user blob."""
    path = os.path.join(BUILD, "user_hello.bin")
    if not os.path.isfile(path):
        raise BuildError("ring-3 hello blob is missing: %s" % rel(path))
    size = os.path.getsize(path)
    if size == 0:
        raise BuildError("ring-3 hello blob is empty")
    return size


def run(cmd, cwd=ROOT, capture=True):
    shown = " ".join(cmd)
    if not QUIET:
        log("+ " + shown)
    try:
        r = subprocess.run(cmd, cwd=cwd, capture_output=capture, text=True,
                           errors="replace")
    except OSError as exc:
        raise BuildError("could not execute %s: %s" % (cmd[0], exc))
    if r.stdout:
        sys.stdout.write(r.stdout)
        sys.stdout.flush()
    if r.stderr:
        sys.stderr.write(r.stderr)
        sys.stderr.flush()
    if r.returncode != 0:
        raise BuildError("command failed (exit %d): %s"
                         % (r.returncode, shown))
    return r


def find_tool(env_name, candidates, version_args, what):
    seen = []
    for path in candidates:
        if not path:
            continue
        path = os.path.abspath(os.path.expandvars(path))
        if path in seen:
            continue
        seen.append(path)
        if not os.path.isfile(path):
            continue
        try:
            r = subprocess.run([path] + version_args, capture_output=True,
                               text=True, timeout=20)
        except (OSError, subprocess.TimeoutExpired):
            continue
        if r.returncode == 0:
            return path, (r.stdout or r.stderr).strip().splitlines()[0]
    raise BuildError(
        "%s not found. Set %s or install it.\nTried:\n  %s"
        % (what, env_name, "\n  ".join(seen))
    )


def locate_toolchain():
    localappdata = os.environ.get("LOCALAPPDATA", "")
    zig_candidates = [
        os.environ.get("ZIG"),
        shutil.which("zig"),
        os.path.join(localappdata, "Microsoft", "WinGet", "Packages"),
        r"C:\Users\wadia\zig\ziglang\zig.exe",
    ]
    # Replace the WinGet package *directory* with every zig.exe under it.
    expanded = []
    for cand in zig_candidates:
        if not cand:
            continue
        if os.path.isdir(cand):
            expanded.extend(sorted(glob.glob(
                os.path.join(cand, "zig.zig_*", "**", "zig.exe"),
                recursive=True)))
        else:
            expanded.append(cand)
    zig, zig_version = find_tool("ZIG", expanded, ["version"], "Zig")

    objcopy_candidates = [
        os.environ.get("OBJCOPY"),
        shutil.which("objcopy"),
        r"C:\msys64\ucrt64\bin\objcopy.exe",
        r"C:\msys64\mingw64\bin\objcopy.exe",
    ]
    objcopy, objcopy_version = find_tool(
        "OBJCOPY", objcopy_candidates, ["--version"], "objcopy")

    return zig, zig_version, objcopy, objcopy_version


def rel(path):
    return os.path.relpath(path, ROOT)


CC64 = None  # filled in by main()


def compile_kernel(zig):
    flags = CC64
    objs = []

    kernel_sources = sorted(glob.glob(os.path.join(ROOT, "kernel", "*.c")))
    lib_sources = sorted(glob.glob(os.path.join(ROOT, "lib", "*.c")))
    if not kernel_sources:
        raise BuildError("no kernel sources found")

    sources = [(src, "k_") for src in kernel_sources]
    sources += [(src, "l_") for src in lib_sources]
    sources += [
        (os.path.join(ROOT, "boot", "entry64.S"), "k_"),
        (os.path.join(ROOT, "kernel", "isr.S"), "k_"),
    ]

    for src, prefix in sources:
        obj = os.path.join(
            BUILD, prefix + os.path.basename(src).rsplit(".", 1)[0] + ".o")
        run([zig, "cc"] + flags + ["-c", src, "-o", obj])
        objs.append(obj)
    return objs


USER_FLAGS = [
    "-target", "x86_64-freestanding",
    "-ffreestanding", "-fno-builtin",
    "-fno-sanitize=all",
    "-fno-asynchronous-unwind-tables", "-fno-unwind-tables",
    "-fno-pic", "-fno-pie", "-fno-stack-protector",
    "-mno-red-zone", "-mno-mmx", "-mno-sse", "-mno-sse2", "-mno-avx",
    "-mcmodel=large", "-O2",
]

CRT0 = os.path.join(ROOT, "userspace", "crt0.S")
SHELL_C = os.path.join(ROOT, "userspace", "shell.c")

# Placeholder until Task 5 adds userspace/hello.c. Building the second blob
# from the shell sources keeps it a valid, nonempty flat binary with the new
# user_main entry point; Task 5 replaces this list with [CRT0, HELLO_C].
USER_HELLO_SOURCES = [CRT0, SHELL_C]


def build_user_blob(zig, objcopy, name, sources):
    """Build one freestanding flat binary and its objcopy symbol object."""
    user_elf = os.path.join(BUILD, name + ".elf")
    user_bin = os.path.join(BUILD, name + ".bin")
    user_obj = os.path.join(BUILD, name + "_bin.o")

    run([zig, "cc"] + USER_FLAGS + [
        "-nostdlib",
        "-Wl,-T," + os.path.join(ROOT, "userspace", "link.ld"),
        "-Wl,-e,_start",
        "-Wl,--build-id=none",
        "-Wl,-z,noexecstack",
        "-o", user_elf,
    ] + sources)
    run([objcopy, "-O", "binary", user_elf, user_bin])

    # Run from BUILD so objcopy derives the plain basename symbols:
    # _binary_<name>_bin_{start,end,size}
    run([objcopy, "-I", "binary", "-O", "elf64-x86-64",
         "-B", "i386:x86-64",
         "--rename-section", ".data=.userblob,alloc,load,readonly,data,contents",
         os.path.basename(user_bin), user_obj], cwd=BUILD)

    size = os.path.getsize(user_bin)
    if size == 0:
        raise BuildError("ring-3 %s blob is empty" % name)
    log("USER BLOB  %s (%d bytes)" % (rel(user_bin), size))
    return user_bin, user_obj


def build_user_shell(zig, objcopy):
    return build_user_blob(zig, objcopy, "user_shell", [CRT0, SHELL_C])


def build_user_hello(zig, objcopy):
    return build_user_blob(zig, objcopy, "user_hello", USER_HELLO_SOURCES)


def link_kernel(zig, objs, user_objs):
    kernel_elf = os.path.join(BUILD, "os_kernel.elf")
    run([zig, "cc"] + CC64 + [
        "-nostdlib",
        "-Wl,-T," + os.path.join(ROOT, "linker", "kernel64.ld"),
        "-Wl,-e,_entry64",
        "-Wl,--build-id=none",
        "-Wl,-z,noexecstack",
        "-o", kernel_elf,
    ] + objs + user_objs)
    return kernel_elf


def make_os_elf(zig, objcopy, kernel_elf):
    kernel_bin = os.path.join(BUILD, "kernel.bin")
    run([objcopy, "-O", "binary", kernel_elf, kernel_bin])

    blob_obj = os.path.join(BUILD, "kernel_blob.o")
    run([objcopy, "-I", "binary", "-O", "elf32-i386", "-B", "i386",
         "--rename-section", ".data=.kernelblob,alloc,load,readonly,data,contents",
         kernel_bin, blob_obj])

    cc32 = [
        "-target", "x86-freestanding",
        "-ffreestanding", "-fno-builtin",
        "-fno-sanitize=all",
        "-fno-asynchronous-unwind-tables", "-fno-unwind-tables",
        "-fno-pic", "-fno-pie", "-fno-stack-protector",
        "-mno-red-zone", "-O2",
    ]
    boot_obj = os.path.join(BUILD, "boot32.o")
    run([zig, "cc"] + cc32 + [
        "-c", os.path.join(ROOT, "boot", "boot32.S"), "-o", boot_obj])

    os_elf = os.path.join(ROOT, "os.elf")
    run([zig, "cc"] + cc32 + [
        "-nostdlib",
        "-Wl,-T," + os.path.join(ROOT, "linker", "boot32.ld"),
        "-Wl,-e,_start",
        "-Wl,--build-id=none",
        "-Wl,--no-gc-sections",
        "-Wl,-z,noexecstack",
        "-o", os_elf, boot_obj, blob_obj])

    log("KERNEL BIN %s (%d bytes)" % (rel(kernel_bin),
                                      os.path.getsize(kernel_bin)))
    return os_elf, kernel_bin


def verify_kernel_elf(path):
    with open(path, "rb") as f:
        hdr = f.read(64)
    if len(hdr) < 64 or hdr[:4] != b"\x7fELF":
        raise BuildError("kernel is not an ELF file")
    if hdr[4] != 2:
        raise BuildError("kernel ELF is not 64-bit")
    e_machine = struct.unpack_from("<H", hdr, 18)[0]
    e_entry = struct.unpack_from("<Q", hdr, 24)[0]
    if e_machine != 62:  # EM_X86_64
        raise BuildError("kernel ELF has wrong machine type %d" % e_machine)
    if e_entry == 0:
        raise BuildError("kernel ELF has a zero entry point")
    log("KERNEL ELF %s (entry 0x%x, %d bytes)"
        % (rel(path), e_entry, os.path.getsize(path)))


def verify_no_vector_code(kernel_elf, objcopy):
    """Reject vector/MMX instructions: the boot path never enables SSE.

    Kernel code runs with CR4.OSFXSR clear, so any xmm/mmx instruction is a
    guaranteed #UD.  A compiler upgrade silently re-enabling auto-vectorization
    would otherwise produce a kernel that dies before the first serial byte.
    """
    objdump = os.path.join(os.path.dirname(objcopy), "objdump.exe")
    if not os.path.isfile(objdump):
        objdump = shutil.which("objdump")
    if not objdump:
        log("WARNING: objdump not found; skipping no-vector-code check")
        return

    r = subprocess.run([objdump, "-d", kernel_elf], capture_output=True,
                       text=True, errors="replace")
    if r.returncode != 0:
        raise BuildError("objdump failed on the kernel image")

    bad = []
    for line in r.stdout.splitlines():
        if any(reg in line for reg in ("%xmm", "%ymm", "%zmm", "%mm")):
            bad.append(line.strip())
    if bad:
        raise BuildError(
            "kernel text contains vector/MMX instructions, but SSE is not "
            "enabled at boot (would #UD):\n  " + "\n  ".join(bad[:5]))
    log("NO-SSE   kernel text has no vector/MMX instructions")


def verify_multiboot_elf(path):
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 64 or data[:4] != b"\x7fELF":
        raise BuildError("os.elf is not an ELF file")
    if data[4] != 1:
        raise BuildError("os.elf must be ELF32 for QEMU/GRUB multiboot")
    e_machine = struct.unpack_from("<H", data, 18)[0]
    if e_machine != 3:  # EM_386
        raise BuildError("os.elf has wrong machine type %d" % e_machine)

    limit = min(len(data) - 12, 8192)
    found = False
    for off in range(0, limit + 1, 4):
        magic, flags, checksum = struct.unpack_from("<III", data, off)
        if magic != MULTIBOOT_MAGIC:
            continue
        if (magic + flags + checksum) & 0xFFFFFFFF:
            raise BuildError("multiboot header at 0x%x has bad checksum" % off)
        found = True
        break
    if not found:
        raise BuildError(
            "no valid multiboot header in the first 8 KiB of os.elf; "
            "QEMU/GRUB will reject the image")
    log("BOOT IMAGE %s (%d bytes, multiboot OK)"
        % (rel(path), len(data)))


def verify_embedded_kernel(os_elf, kernel_bin):
    with open(os_elf, "rb") as f:
        data = f.read()
    with open(kernel_bin, "rb") as f:
        kernel = f.read()
    if not kernel:
        raise BuildError("kernel blob is empty")

    e_phoff = struct.unpack_from("<I", data, 28)[0]
    e_phentsize = struct.unpack_from("<H", data, 42)[0]
    e_phnum = struct.unpack_from("<H", data, 44)[0]
    if e_phentsize < 32 or e_phnum == 0:
        raise BuildError("os.elf has no program headers")

    need_start = KERNEL_LOAD
    need_end = KERNEL_LOAD + len(kernel)
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        p_type, p_offset, p_vaddr, p_paddr, p_filesz, _ = struct.unpack_from(
            "<IIIIII", data, off)
        if p_type != 1:  # PT_LOAD
            continue
        if p_paddr <= need_start and p_paddr + p_filesz >= need_end:
            src = p_offset + (need_start - p_vaddr)
            if src < 0 or src + len(kernel) > len(data):
                raise BuildError("embedded kernel segment is truncated")
            if data[src:src + len(kernel)] != kernel:
                raise BuildError(
                    "embedded kernel bytes differ from kernel.bin")
            log("EMBEDDED  %d-byte kernel present at 0x%x"
                % (len(kernel), need_start))
            return
    raise BuildError(
        "os.elf has no PT_LOAD covering [0x%x, 0x%x); the boot image "
        "does not contain the kernel" % (need_start, need_end))


def main(argv):
    global CC64, QUIET

    clean = "--clean" in argv
    keep_build = "--keep-build" in argv
    QUIET = "--quiet" in argv
    allowed = {"--clean", "--keep-build", "--quiet"}
    for arg in argv:
        if arg not in allowed:
            log("unknown argument: %s" % arg)
            return 2
    if clean and keep_build:
        log("--clean and --keep-build cannot be combined")
        return 2

    zig, zig_version, objcopy, objcopy_version = locate_toolchain()
    log("ARC OS build")
    log("  zig:     %s (%s)" % (zig, zig_version))
    log("  objcopy: %s (%s)" % (objcopy, objcopy_version))
    log("  build:   %s" % BUILD)

    if clean:
        shutil.rmtree(BUILD, ignore_errors=True)
    os.makedirs(BUILD, exist_ok=True)
    if not keep_build:
        for stale in glob.glob(os.path.join(BUILD, "*.o")):
            os.remove(stale)
    # Never leave a previous image that could be mistaken for this build.
    for stale in (os.path.join(ROOT, "os.elf"),
                  os.path.join(BUILD, "kernel.bin"),
                  os.path.join(BUILD, "os_kernel.elf")):
        if os.path.exists(stale):
            os.remove(stale)

    CC64 = [
        "-target", "x86_64-freestanding",
        "-ffreestanding", "-fno-builtin",
        "-fno-sanitize=all",
        "-fno-asynchronous-unwind-tables", "-fno-unwind-tables",
        "-fno-pic", "-fno-pie", "-fno-stack-protector",
        "-fno-omit-frame-pointer",
        "-mno-mmx", "-mno-sse", "-mno-sse2", "-mno-avx",
        "-mno-red-zone", "-O2",
        "-I" + os.path.join(ROOT, "kernel"),
        "-I" + os.path.join(ROOT, "lib"),
    ]

    log("\n[1/5] compiling kernel and libc objects")
    objs = compile_kernel(zig)

    log("\n[2/5] building ring-3 user blobs")
    _, shell_obj = build_user_shell(zig, objcopy)
    _, hello_obj = build_user_hello(zig, objcopy)
    verify_user_hello_blob()

    log("\n[3/5] linking 64-bit kernel")
    kernel_elf = link_kernel(zig, objs, [shell_obj, hello_obj])
    verify_kernel_elf(kernel_elf)
    verify_no_vector_code(kernel_elf, objcopy)

    log("\n[4/5] embedding kernel in 32-bit boot image")
    os_elf, kernel_bin = make_os_elf(zig, objcopy, kernel_elf)
    verify_multiboot_elf(os_elf)
    verify_embedded_kernel(os_elf, kernel_bin)

    log("\n[5/5] BUILD OK -> %s" % os_elf)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except BuildError as exc:
        print("\nBUILD FAILED: %s" % exc, file=sys.stderr)
        sys.exit(1)
