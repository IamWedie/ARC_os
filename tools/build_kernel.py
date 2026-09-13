import os, subprocess, sys, glob

ROOT = r"C:/Users/wadia/os_project"
BUILD = r"C:/Users/wadia/AppData/Local/Temp/opencode/build"
ZIG = r"C:/Users/wadia/AppData/Local/Microsoft/WinGet/Links/zig.exe"
OBJCOPY = r"C:/msys64/ucrt64/bin/objcopy.exe"

os.makedirs(BUILD, exist_ok=True)

def run(cmd):
    print('+ ' + ' '.join(cmd))
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if r.stdout: print(r.stdout, end='')
    if r.stderr: sys.stderr.write(r.stderr)
    if r.returncode != 0:
        print('FAILED (exit %d)' % r.returncode)
        sys.exit(1)

# 64-bit kernel compile flags
CC64 = [ZIG, 'cc', '-target', 'x86_64-freestanding', '-ffreestanding',
        '-fno-builtin', '-fno-sanitize=all', '-fno-asynchronous-unwind-tables',
        '-fno-unwind-tables', '-mno-red-zone', '-Ikernel']

# ---------- Stage 1: compile 64-bit kernel objects ----------
objs64 = []
for src in (['kernel/%s.c' % s for s in
             ['kernel', 'console', 'gdt', 'idt', 'interrupts',
              'memory', 'fs', 'scheduler', 'syscall', 'pmm', 'vmm',
              'timer', 'user']]
            + ['boot/entry64.S', 'kernel/isr.S']):
    name = os.path.basename(src).replace('.c', '.o').replace('.S', '.o')
    out = os.path.join(BUILD, 'k_' + name)
    run(CC64 + ['-c', src, '-o', out])
    objs64.append(out)

# ---------- Stage 1b: build the ring-3 user shell binary ----------
CCU = [ZIG, 'cc', '-target', 'x86_64-freestanding', '-ffreestanding',
       '-fno-builtin', '-fno-sanitize=all', '-fno-asynchronous-unwind-tables',
       '-fno-unwind-tables', '-mno-red-zone', '-nostdlib']
user_elf = os.path.join(BUILD, 'user_shell.elf')
run(CCU + ['-Wl,-T,%s' % os.path.join(ROOT, 'userspace/link.ld'),
           '-Wl,-e,_start',
           '-o', user_elf,
           os.path.join(ROOT, 'userspace/crt0.S'),
           os.path.join(ROOT, 'userspace/shell.c')])
user_bin = os.path.join(BUILD, 'user_shell.bin')
run([OBJCOPY, '-O', 'binary', user_elf, user_bin])
print('USER BLOB -> %s (%d bytes)' % (user_bin, os.path.getsize(user_bin)))

# embed the user binary into the 64-bit kernel ELF as a .userblob object
# (run from BUILD so the _binary_* symbols derive from the basename only)
userblob = os.path.join(BUILD, 'user_shell_bin.o')
r = subprocess.run([OBJCOPY, '-I', 'binary', '-O', 'elf64-x86-64', '-B', 'i386:x86-64',
                    '--rename-section', '.data=.userblob',
                    'user_shell.bin', userblob],
                   cwd=BUILD, capture_output=True, text=True)
if r.stdout: print(r.stdout, end='')
if r.stderr: sys.stderr.write(r.stderr)
if r.returncode != 0:
    print('FAILED (exit %d)' % r.returncode)
    sys.exit(1)
objs64.append(userblob)

# ---------- Stage 2: link 64-bit kernel ELF ----------
kernelf = os.path.join(BUILD, 'os_kernel.elf')
run(CC64 + ['-nostdlib',
            '-Wl,-T,%s' % os.path.join(ROOT, 'linker/kernel64.ld'),
            '-Wl,-e,_entry64',
            '-o', kernelf] + objs64)

# ---------- Stage 3: objcopy kernel ELF -> raw binary blob ----------
kbin = os.path.join(ROOT, 'kernel.bin')
run([OBJCOPY, '-O', 'binary', kernelf, kbin])
print('KERNEL BLOB -> %s (%d bytes)' % (kbin, os.path.getsize(kbin)))

# ---------- Stage 4: embed kernel blob as object + assemble/link stub ----------
CC32 = [ZIG, 'cc', '-target', 'x86-freestanding', '-ffreestanding',
        '-fno-builtin', '-fno-sanitize=all', '-fno-asynchronous-unwind-tables',
        '-fno-unwind-tables']

# convert raw kernel blob to an ELF32 object (its .kernelblob section is
# linked at 0x200000 by boot32.ld). Done via objcopy (not zig assembler)
# to avoid zig's compile cache going stale on the embedded blob.
blobobj = os.path.join(BUILD, 'kernel_blob.o')
run([OBJCOPY, '-I', 'binary', '-O', 'elf32-i386', '-B', 'i386',
     '--rename-section', '.data=.kernelblob', kbin, blobobj])

boot32o = os.path.join(BUILD, 'boot32.o')
run(CC32 + ['-c', 'boot/boot32.S', '-o', boot32o])

oself = os.path.join(ROOT, 'os.elf')
run(CC32 + ['-nostdlib',
            '-Wl,-T,%s' % os.path.join(ROOT, 'linker/boot32.ld'),
            '-Wl,-e,_start',
            '-o', oself, boot32o, blobobj])
print('BOOT IMAGE -> %s (%d bytes)' % (oself, os.path.getsize(oself)))