#include "kernel.h"

extern void gdt_init(void);
extern void idt_init(void);
extern void interrupts_init(void);
extern void scheduler_init(void);
extern void fs_init(void);

void kernel_main(uint64_t multiboot_addr) {
    terminal_initialize();

    terminal_putstring("=== ARC OS v1.0 ===\n");
    terminal_putstring("Full OS from scratch\n");
    terminal_putstring("Booting...\n");

    /* Physical + virtual memory first: everything depends on real RAM. */
    pmm_init(multiboot_addr);
    vmm_init();

    terminal_putstring("Physical memory: ");
    terminal_print_int(pmm_total_mem() >> 20);
    terminal_putstring(" MiB total, ");
    terminal_print_int(pmm_free_mem() >> 20);
    terminal_putstring(" MiB available\n");

    /* Initialize all subsystems */
    gdt_init();
    idt_init();
    interrupts_init();
    scheduler_init();
    pit_init();
    fs_init();
    
    /* Print system info */
    terminal_putstring("Kernel loaded at ");
    terminal_print_hex(0x100000);
    terminal_putstring("\n");
    terminal_putstring("VGA console initialized\n");
    terminal_putstring("Keyboard driver initialized\n");
    terminal_putstring("Process scheduler initialized (100 Hz)\n");
    terminal_putstring("Filesystem initialized\n");

    if (mem_selftest() == 0)
        terminal_putstring("Heap self-test OK\n");
    else
        terminal_putstring("Heap self-test FAILED\n");

    terminal_putstring("\n--- ARC OS Shell ---\n");

    /* Start the userspace shell (ring 3) as its own preemptible process. */
    if (user_process_create_from_blob() < 0)
        terminal_putstring("FAILED: user shell could not be loaded\n");

    /* Process 0 (kernel) becomes the idle loop. */
    while (1) {
        __asm__ volatile("hlt");
    }
}
