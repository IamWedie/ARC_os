#include "kernel.h"
#include "gfx.h"

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
    /* Copy by value: taking a pointer into a packed struct is unaligned. */
    const uint64_t r[15] = {
        f->r15, f->r14, f->r13, f->r12, f->rbp, f->rbx, f->r11,
        f->r10, f->r9, f->r8, f->rdi, f->rsi, f->rdx, f->rcx, f->rax
    };
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

static void fmt_hex64(char* out, uint64_t v) {
    static const char hex[] = "0123456789abcdef";
    out[0] = '0'; out[1] = 'x';
    for (int i = 15; i >= 0; i--) {
        out[2 + (15 - i)] = hex[(v >> (i * 4)) & 0xF];
    }
    out[18] = '\0';
}

static void fmt_dec(char* out, uint64_t n) {
    char tmp[24];
    int i = 0;
    if (n == 0) { out[0] = '0'; out[1] = '\0'; return; }
    while (n > 0 && i < 23) { tmp[i++] = '0' + (n % 10); n /= 10; }
    int j = 0;
    while (i > 0) out[j++] = tmp[--i];
    out[j] = '\0';
}

static void panic_puts_gfx(int* y, int x, const char* s, uint32_t color) {
    const struct gfx_font* f = gfx_font_get(14);
    if (!f || !s) return;
    int lh = (int)f->line_height;
    struct gfx_fb* fb = gfx_fb_get();
    if (fb && *y + lh > (int)fb->height) return;
    gfx_draw_text(f, x, *y, s, color);
    *y += lh;
}

static void panic_kv_gfx(int* y, int x, const char* k, uint64_t v, uint32_t color) {
    char line[128];
    char hv[20];
    int i = 0;
    fmt_hex64(hv, v);
    while (k[i] && i < 40) { line[i] = k[i]; i++; }
    line[i++] = '=';
    for (int j = 0; hv[j] && i < 127; j++, i++) line[i] = hv[j];
    line[i] = '\0';
    panic_puts_gfx(y, x, line, color);
}

static void gfx_panic_screen(exc_frame_t* f) {
    if (!gfx_ready()) return;
    struct gfx_fb* fb = gfx_fb_get();
    if (!fb) return;
    gfx_clear(gfx_rgba(60, 0, 0, 255));
    uint32_t white = gfx_rgba(255, 255, 255, 255);
    uint32_t yellow = gfx_rgba(255, 230, 120, 255);
    uint32_t gray = gfx_rgba(220, 220, 220, 255);
    int y = 20;
    panic_puts_gfx(&y, 20, "=== ARC OS KERNEL PANIC ===", white);
    y += 4;

    uint32_t v = (uint32_t)(f->vec & 0xFF);
    char hdr[160];
    char num[24];
    int p = 0;
    const char* name = exc_names[v < 32 ? v : 0];
    const char* pre = "Exception: ";
    while (*pre && p < 150) hdr[p++] = *pre++;
    while (*name && p < 150) hdr[p++] = *name++;
    const char* mid = "  vector=";
    while (*mid && p < 150) hdr[p++] = *mid++;
    fmt_dec(num, v); for (int i = 0; num[i] && p < 150; i++) hdr[p++] = num[i];
    const char* mid2 = "  err=";
    while (*mid2 && p < 150) hdr[p++] = *mid2++;
    fmt_hex64(num, f->err); /* reuse buf, but fmt_hex64 writes 19 chars */ 
    { char hv[20]; fmt_hex64(hv, f->err); for (int i = 0; hv[i] && p < 159; i++) hdr[p++] = hv[i]; }
    hdr[p] = '\0';
    panic_puts_gfx(&y, 20, hdr, yellow);

    int user = (f->cs & 0x3) == 0x3;
    panic_puts_gfx(&y, 20, user ? "Mode: user" : "Mode: kernel", gray);
    panic_kv_gfx(&y, 20, "rip", f->rip, gray);
    panic_kv_gfx(&y, 20, "cs", f->cs, gray);
    panic_kv_gfx(&y, 20, "rflags", f->rflags, gray);
    if (user) { panic_kv_gfx(&y, 20, "rsp", f->rsp, gray); panic_kv_gfx(&y, 20, "ss", f->ss, gray); }
    else { panic_kv_gfx(&y, 20, "trap_frame", (uint64_t)f, gray); }
    if (v == 14) {
        uint64_t cr2 = 0;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        panic_kv_gfx(&y, 20, "cr2", cr2, yellow);
    }

    y += 4;
    panic_puts_gfx(&y, 20, "Registers:", white);
    const uint64_t r[15] = {f->r15,f->r14,f->r13,f->r12,f->rbp,f->rbx,f->r11,f->r10,f->r9,f->r8,f->rdi,f->rsi,f->rdx,f->rcx,f->rax};
    static const char* rn[15] = {"r15","r14","r13","r12","rbp","rbx","r11","r10","r9","r8","rdi","rsi","rdx","rcx","rax"};
    for (int i = 0; i < 15; i += 2) {
        char line[128]; char h1[20], h2[20]; int lp = 0;
        fmt_hex64(h1, r[i]); fmt_hex64(h2, r[i+1 < 15 ? i+1 : i]);
        const char* a = rn[i]; while (*a && lp < 120) line[lp++] = *a++;
        line[lp++] = '='; for (int j=0; h1[j] && lp<120; j++) line[lp++]=h1[j];
        if (i+1 < 15) {
            const char* sp = "  "; while(*sp && lp<120) line[lp++]=*sp++;
            const char* b = rn[i+1]; while (*b && lp<120) line[lp++]=*b++;
            line[lp++]='='; for (int j=0; h2[j] && lp<120; j++) line[lp++]=h2[j];
        }
        line[lp]='\0';
        panic_puts_gfx(&y, 20, line, gray);
    }

    y += 4;
    panic_puts_gfx(&y, 20, "Backtrace:", white);
    uint64_t rbp = f->rbp;
    for (int i = 0; i < 12 && rbp; i++) {
        if (rbp < 0x100000 || rbp >= 0x20000000) break;
        uint64_t ret = *(uint64_t*)(rbp + 8);
        uint64_t next = *(uint64_t*)rbp;
        if (ret < (uint64_t)__kernel_start || ret >= (uint64_t)__kernel_end) break;
        char line[128], hv[20], dn[24]; int lp = 0;
        const char* hash = "#"; while(*hash) line[lp++]=*hash++;
        fmt_dec(dn, (uint64_t)i); for(int j=0; dn[j]&&lp<120;j++) line[lp++]=dn[j];
        line[lp++]=' '; line[lp++]=' ';
        fmt_hex64(hv, ret); for(int j=0; hv[j]&&lp<127;j++) line[lp++]=hv[j];
        line[lp]='\0';
        panic_puts_gfx(&y, 20, line, gray);
        if (next <= rbp) break;
        rbp = next;
    }
    panic_puts_gfx(&y, 20, "System halted. Serial/VGA log may have more details.", yellow);
    gfx_present(NULL);
}

void exception_handler(void* fptr) {
    exc_frame_t* f = (exc_frame_t*)fptr;

    /* A second fault while panicking would trash the report; spin instead. */
    if (exc_active) {
        for (;;) __asm__ volatile("cli; hlt");
    }
    exc_active = 1;
    gfx_debug_suspend();

    gfx_panic_screen(f);

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