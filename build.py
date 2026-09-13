import subprocess, os, sys

os.chdir('C:/Users/wadia/os_project')
cc = 'C:/msys64/ucrt64/bin/gcc.exe'

sources = [
    ('boot/entry.S', True),
    ('kernel/kernel.c', False),
    ('kernel/console.c', False),
    ('kernel/gdt.c', False),
    ('kernel/idt.c', False),
    ('kernel/interrupts.c', False),
    ('kernel/scheduler.c', False),
    ('kernel/memory.c', False),
    ('kernel/syscall.c', False),
]

objs = []
for src, is_asm in sources:
    obj = src.replace('.S', '.o').replace('.c', '.o')
    print(f'Compiling {src}...')
    if is_asm:
        cmd = [cc, '-ffreestanding', '-nostdlib', '-m64', '-mno-red-zone', '-c', src, '-o', obj]
    else:
        cmd = [cc, '-ffreestanding', '-nostdlib', '-m64', '-mno-red-zone', '-fno-stack-protector', '-fno-pie', '-I.', '-c', src, '-o', obj]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(f'  ERROR: {r.stderr[:500]}')
        sys.exit(1)
    else:
        print(f'  OK: {obj}')
        objs.append(obj)

print(f'\nObjects: {objs}')
print('Linking...')

link_cmd = cc + ' -nostdlib -ffreestanding -m64 -Wl,-belf64-x86-64 -T linker/linker.ld -o kernel.elf ' + ' '.join(objs)
r = subprocess.run(link_cmd, capture_output=True, text=True, shell=True)
if r.returncode != 0:
    print(f'Link ERROR: {r.stderr[:500]}')
    sys.exit(1)
else:
    print('kernel.elf created!')
    print(f'Size: {os.path.getsize("kernel.elf")} bytes')
