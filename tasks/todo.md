# TODO

## Open findings: M0.5 §2 kernel safety

- [x] VERIFIED FIXED in working tree (one-token mask change; proof: tests/test_vmm_pde_mask.c) -- HIGH PRIORITY -- schedule ahead of the remaining M0.5 section 2 pointer-routing work: kernel/vmm.c:51 PDE address mask (0xFFE00000) drops physical address bits >= 32, aliasing any 2 MiB-aligned PA at or above 4 GiB to the wrong physical page. PMM can allocate up to 16 GiB (kernel/pmm.c:13). Confirmed via real captured QEMU map entry 6 (reserved, base >= 4 GiB). This is not only an identity-map coverage bug: copy_from_user/copy_to_user (kernel/user_access.c:123-124,153-154) translate a validated user VA to PA from the leaf and dereference it through that identity map, so a user page backed by a frame >= 4 GiB can make even an already-validated syscall (including SYS_READ/SYS_WRITE) read or write the wrong physical page, potentially kernel memory. Needs its own design note and fix; do not fix as part of the item-5 syscall pointer routing work.
- [ ] The readok QEMU gate proves copy_to_user() with length > 1, but the 8-byte stack buffer does not straddle a page boundary in observed runs, so ua_validate's cross-page loop iteration is not deterministically exercised by this gate.
- [ ] The readok hook's short-read loop (for when the UART ring returns fewer bytes than requested) is implemented but has not been exercised in any observed run -- every run so far returned the full 6 bytes in one copy_to_user() call.
- [ ] `kernel/vmm.c:16,50,78` caps the kernel identity map at `VMM_PD_COUNT = 128` (128 GiB). The captured real multiboot map has a reserved entry at `0xFD00000000` (~1 TiB), above this ceiling, so `vmm_init()` skips it and any kernel access to firmware/ACPI data above 128 GiB through the identity map is unsupported. Needs its own design note and fix; not addressed by the PDE mask fix.
- [ ] `kernel/vmm.c:62-66` claims both AVAILABLE and reserved multiboot regions are identity-mapped, but `vmm_init()` iterates `pmm_regions[]` (`kernel/vmm.c:67-81`) and `kernel/pmm.c:93-111` records only AVAILABLE entries; reserved entries are used only to re-reserve frames. Reserved-region coverage is a documentation/implementation mismatch and is only incidental where a rounded-down AVAILABLE 2 MiB page overlaps it. Needs its own design note and fix; not addressed by the PDE mask fix.

## Current: M1 — 1080p framebuffer, font, double buffering

- [x] M1 design note + DISPI/MMIO spike confirmation (ADR-007, risk-log)
- [x] Step 1: gfx_init max-res detect/readback, 1920x1080x32 request, fallbacks
- [x] Step 2: QEMU args `-vga none -device bochs-display,vgamem=67108864` (MMIO BAR2 fallback)
- [x] Step 3: RAM back buffer from PMM
- [x] Step 4: gfx_present dirty-rect copy + merge (host-testable)
- [x] Step 5: OFL Inter pre-rasterized font (12/14/18/24), coverage alpha blend
- [x] Step 6: premultiplied-alpha blend/fill primitives (scalar)
- [x] Step 7: 60 Hz LAPIC frame pacer, skip clean frames
- [x] Host tests: tests/test_gfx.c suite 12
- [x] QEMU gate: tests/boot_test_gui.py screendump pixel assertions
- [x] All gates green, commit, review, merge/tag

## Current: M2 — Mouse, compositor, windows, dock, menu bar (kernel-mode)

- [x] Step 1: PS/2 mouse driver (IRQ12, 3-byte packets, clamp, accel) + host tests
- [x] Step 2: compositor core (16 windows, Z-order, premul blend, damage, hit-test)
- [x] Step 3: basic visual layer (rounded corners, title bar, traffic lights)
- [x] Step 4: menu bar (translucent, 28 px, logo/app name/clock)
- [x] Step 5: dock (centered pill, hover magnification, running indicator)
- [x] Step 6: animations (ease-out/spring open/close/minimize)
- [x] Step 7: built-in apps (About, Notes, Files, System Monitor)
- [x] Step 8: keyboard routing (focused text field gets keys, else shell)
- [x] Host tests: tests/test_gui.c suite 13 extended
- [x] QEMU gate: tests/boot_test_gui.py desktop screendump assertions
- [x] M2 interaction gate: mouse move/click/window appear/drag/close/dock hover
- [x] All M2 gates green, commit, review, merge/tag

## Done: M3 — Real-hardware boot path

- [x] MB2+MB1 headers/parser, normalized bootinfo, boot_fb+bochs_dispi fallback, PMM hardening, on-screen debug/panic, USB image builder, boot_test_mb2, host bootinfo tests. Gates green. Integrated to main, tagged `m3`.

## Current: M4 — Persistent storage

- [x] M4.1 PCI enumeration (`kernel/pci.c`): scan buses, record vendor/device/class/BARs/IRQ
- [x] M4.2 Block layer (`kernel/blockdev.c`): read/write/flush, small write-through block cache
- [x] M4.3 Wrap existing ATA PIO in block interface; keep as QEMU baseline
- [x] M4.4 AHCI A-E done: controller detection/BAR5/IRQ, DMA read/write, FAT32 ARCROOT boot policy, read-only policy for non-QEMU models. Sub-step F DEFERRED (optional polling completion; production path is polling).
- [x] M4.5 Partition tables: MBR/GPT parsing (read-only)
- [x] M4.6 FAT32: read, write, directories, SFN + LFN read/write
- [x] M4.7 VFS: mount table, path resolution, vfs ops dispatch; ARCFS and FAT32 plug in; fd layer unchanged
- [x] M4.8 Syscalls 17+: SYS_STAT, SYS_READDIR, SYS_MKDIR, SYS_UNLINK, SYS_MOUNT; documented in `docs/specs/syscall-abi.md`
- [x] M4.9 Boot/load first user app from disk feasible: FAT32-backed FS dispatch + embedded hello.bin installed to mounted FS; ARCFS RAM recovery path retained when no disk
- [x] M4.10a Flush contract done: syscall 22 `SYS_SYNC`, blockdev flush plumbing, durability spec; full journaling remains deferred
- [ ] M4.10b next: durability/recovery follow-up (not started)
- [x] Host tests: FAT32 90/90, sync 17/17, AHCI 204/204, storage policy 15/15, plus slab/scheduler/fd/fs/page/as/fork/exec/gui/bootinfo suites
- [x] QEMU gates: `boot_test_persist`, `boot_test_persist_ahci`, `boot_test_fork`, `boot_test_exec`, `boot_test_ahci`, `boot_test_ahci_mixed`, `boot_test_ahci_readonly` all PASS
- [~] Real-hardware/VM read-only test pending on owner hardware: VBoxManage absent here; see `docs/vm-readonly-test.md`. QEMU equivalent with `model=VBOX-LIKE-DISK` PASS and recorded in `tasks/hardware-log.md`.
- [x] Required gates green on final commit: `run_tests.bat`, `build_kernel.py --quiet`, `run_all.bat` (QUALITY GATE PASSED)
- [x] Commits, tags, and push done on `m4-persistent-storage`; M4.4 F and M4.10b not started
