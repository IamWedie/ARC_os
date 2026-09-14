#include "kernel.h"

/*
 * CPU exception handling (utility vectors 0..31). Each IDT gate jumps to an
 * assembly stub (isr.S) that keeps the hardware error code (or pushes a
 * dummy), pushes the vector number, saves all GPRs and calls us with a
 * pointer to that frame. We print the CPU state then halt. The stubs never
 * return, so an unwind of the interrupted code is impossible on purpose.
 */

typedef struct {
    uint64_t r15, r14, r13, r12, rbp, rbx, r11, r10, r9, r8;
    uint64_t rdi, rsi, rdx, rcx, rax;
    uint64_t vec;      /* vector number (pushed by the stub)     */
    uint64_t err;      /* error code (hardware or dummy 0)       */
    uint64_t rip, cs, rflags, rsp, ss;   /* CPU-pushed frame     */
} __attribute__((packed)) exc_frame_t;

extern uint64_t exc_stub_table[32];

static const char* exc_names[32] = {
    "#DE divide error",        "#DB debug",
    "NMI",                     "#BP breakpoint",
    "#OF overflow",            "#BR bound range exceeded",
    "#UD invalid opcode",      "#NM no FPU",
    "#DF double fault",        "reserved (9)",
    "#TS invalid TSS",         "#NP segment not present",
    "#SS stack segment fault", "#GP general protection",
    "#PF page fault",          "reserved (15)",
    "#MF x87 FPU",             "#AC alignment check",
    "#MC machine check",       "#XF SIMD FPU",
    "#VE virtualization",      "#CP control protection",
    "reserved (22)",           "reserved (23)",
    "reserved (24)",           "reserved (25)",
    "reserved (26)",           "reserved (27)",
    "reserved (28)",           "reserved (29)",
    "#SX security exception",  "reserved (31)"
};

static int exc_active = 0;

void exception_gate_init(void) {
    for (int i = 0; i < 32; i++)
        idt_set_gate((uint8_t)i, exc_stub_table[i], 0x08, 0x8E);
}

static void pn(const char* label, uint64_t v) {
    terminal_putstring(label);
    terminal_putstring("=");
    terminal_print_hex(v);
    terminal_putstring("\n");
}

static void dump_gprs(const exc_frame_t* f) {
    const uint64_t* r = &f->r15;
    static const char* name[15] = {
        "r15", "r14", "r13", "r12", "rbp", "rbx", "r11",
        "r10", "r9", "r8", "rdi", "rsi", "rdx", "rcx", "rax"
    };
    for (int i = 0; i < 15; i += 2) {
        terminal_putstring("  ");
        terminal_putstring(name[i]);
        terminal_putstring("=");
        terminal_print_hex(r[i]);
        if (i + 1 < 15) {
            terminal_putstring("   ");
            terminal_putstring(name[i + 1]);
            terminal_putstring("=");
            terminal_print_hex(r[i + 1]);
        }
        terminal_putstring("\n");
    }
}

static void dump_trace(const exc_frame_t* f) {
    terminal_putstring("Backtrace (rbp chain):\n");
    uint64_t rbp = f->rbp;
    for (int i = 0; i < 16 && rbp; i++) {
        if (rbp < 0x100000 || rbp >= 0x20000000) break;
        uint64_t ret = *(uint64_t*)(rbp + 8);
        uint64_t next = *(uint64_t*)rbp;
        if (ret < (uint64_t)__kernel_start || ret >= (uint64_t)__kernel_end) break;
        terminal_putstring("  #");
        terminal_print_int(i);
        terminal_putstring("  ");
        terminal_print_hex(ret);
        terminal_putstring("\n");
        if (next <= rbp) break;   /* frames must unwind towards higher addrs */
        rbp = next;
    }
}

static void dump_stack(const exc_frame_t* f) {
    terminal_putstring("Stack dump (16 qwords around trap frame):\n");
    const uint64_t* p = (const uint64_t*)f;
    for (int i = 0; i < 16; i++) {
        terminal_putstring("  ");
        terminal_print_hex((uint64_t)&p[i]);
        terminal_putstring(": ");
        terminal_print_hex(p[i]);
        terminal_putstring("\n");
    }
}

void exception_handler(void* fptr) {
    exc_frame_t* f = (exc_frame_t*)fptr;

    /* A second fault while panicking would trash the report; spin instead. */
    if (exc_active) {
        for (;;) __asm__ volatile("cli; hlt");
    }
    exc_active = 1;

    terminal_setcolor(COLOR_WHITE | (COLOR_RED << 4));
    terminal_putstring("\n=== KERNEL EXCEPTION ===\n");
    terminal_setcolor(COLOR_LIGHT_GREY | (COLOR_BLUE << 4));

    uint32_t v = (uint32_t)(f->vec & 0xFF);
    terminal_putstring(exc_names[v < 32 ? v : 32]);
    terminal_putstring("  vector=");
    terminal_print_int(v);
    terminal_putstring("  error=");
    terminal_print_hex(f->err);
    terminal_putstring("\n");

    int user = (f->cs & 0x3) == 0x3;
    terminal_putstring("Mode: ");
    terminal_putstring(user ? "user" : "kernel");
    terminal_putstring("\n");

    pn("rip", f->rip);
    pn("cs", f->cs);
    pn("rflags", f->rflags);
    if (user) {
        pn("rsp", f->rsp);
        pn("ss", f->ss);
    } else {
        pn("trap_frame", (uint64_t)f);
    }
    if (v == 14) {
        uint64_t cr2 = 0;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        pn("cr2 (faulting address)", cr2);
    }

    terminal_putstring("Registers:\n");
    dump_gprs(f);
    dump_trace(f);
    dump_stack(f);

    terminal_putstring("System halted\n");
    for (;;) __asm__ volatile("cli; hlt");
}