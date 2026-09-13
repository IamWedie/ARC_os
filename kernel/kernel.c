#include "kernel.h"

extern void gdt_init(void);
extern void idt_init(void);
extern void interrupts_init(void);
extern void scheduler_init(void);
extern void scheduler_yield(void);
extern void fs_init(void);

void kernel_main(uint64_t multiboot_addr) {
    terminal_initialize();
    
    terminal_putstring("=== ARC OS v1.0 ===\n");
    terminal_putstring("Full OS from scratch\n");
    terminal_putstring("Booting...\n");
    
    /* Initialize all subsystems */
    gdt_init();
    idt_init();
    interrupts_init();
    scheduler_init();
    fs_init();
    
    /* Print system info */
    terminal_putstring("Kernel loaded at ");
    terminal_print_hex(0x100000);
    terminal_putstring("\n");
    terminal_putstring("VGA console initialized\n");
    terminal_putstring("Keyboard driver initialized\n");
    terminal_putstring("Process scheduler initialized\n");
    terminal_putstring("Filesystem initialized\n");
    terminal_putstring("\n--- ARC OS Shell ---\n");
    
    /* Start userspace shell */
    shell_main();
    
    /* Should not reach here */
    while(1) {
        __asm__ volatile("hlt");
    }
}
