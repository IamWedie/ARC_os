#include "kernel.h"
#include "gfx.h"

extern void gdt_init(void);
extern void idt_init(void);
extern void interrupts_init(void);
extern void scheduler_init(void);
extern void fs_init(void);

void kernel_main(uint64_t multiboot_addr) {
    terminal_initialize();
    serial_init();

    terminal_putstring("=== ARC OS v1.0 ===\n");
    terminal_putstring("Full OS from scratch\n");
    terminal_putstring("Booting...\n");

    /* Physical + virtual memory first: everything depends on real RAM. */
    pmm_init(multiboot_addr);
    vmm_init();
    kmem_init_early();

    if (gfx_init() == 0) {
        struct gfx_fb* fb = gfx_fb_get();
        /* Gradient wallpaper: top (20,40,90) to bottom (40,80,140). */
        for (uint32_t y = 0; y < fb->height; y++) {
            uint8_t r = (uint8_t)(20 + (20 * y) / fb->height);
            uint8_t g = (uint8_t)(40 + (40 * y) / fb->height);
            uint8_t b = (uint8_t)(90 + (50 * y) / fb->height);
            gfx_fill_rect(0, (int)y, (int)fb->width, 1, gfx_rgba(r, g, b, 255));
        }
        /* Test rectangle and anti-aliased text for M1 gate. */
        gfx_fill_rect(100, 100, 200, 100, gfx_rgba(200, 40, 40, 255));
        gfx_draw_text(gfx_font_get(24), 100, 250, "ARC OS M1", gfx_rgba(255, 255, 255, 255));
        uint32_t t0_lo, t0_hi, t1_lo, t1_hi;
        __asm__ volatile("rdtsc" : "=a"(t0_lo), "=d"(t0_hi));
        gfx_present(NULL);
        __asm__ volatile("rdtsc" : "=a"(t1_lo), "=d"(t1_hi));
        gfx_dirty_reset();
        uint64_t t0 = ((uint64_t)t0_hi << 32) | t0_lo;
        uint64_t t1 = ((uint64_t)t1_hi << 32) | t1_lo;
        terminal_putstring("GFX: frame redraw cycles=");
        terminal_print_int(t1 - t0);
        terminal_putstring("\n");
    }

    acpi_init();

    terminal_putstring("Physical memory: ");
    terminal_print_int(pmm_total_mem() >> 20);
    terminal_putstring(" MiB total, ");
    terminal_print_int(pmm_free_mem() >> 20);
    terminal_putstring(" MiB available\n");

    /* Initialize all subsystems */
    gdt_init();
    idt_init();
    interrupts_init();
    lapic_init();
    lapic_timer_start();
    /* Route device IRQs through the IOAPIC (masks the PIC + ExtINT). */
    ioapic_init();
    /* The PIT now drives IRQ0 as a background clock; the LAPIC is the tick. */
    pit_init();
    /* Mask PIT IRQ0 in the PIC (no-op once the IOAPIC disabled the PIC). */
    outb(0x21, 0x01);
    scheduler_init();
    ata_init();
    fs_init();
    exec_install_programs();
    
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

    /*
     * Hand the CPU to the first runnable process through the software-yield
     * gate. This proves yield_stub works and avoids waiting for a timer tick.
     */
    scheduler_yield();

    /* Process 0 (kernel) becomes the idle loop and resource reaper. */
    while (1) {
        scheduler_reap_zombies();
        __asm__ volatile("hlt");
    }
}
