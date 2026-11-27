/*
 * ARC OS - centralized kernel panic and CPU fault diagnostics.
 *
 * This file owns the one fatal entry point used by kernel-mode exceptions and
 * by unrecoverable kernel errors. It deliberately uses only static state,
 * bounded stacks and polled COM1 writes: no allocation, locks, scheduler,
 * filesystem, process cleanup or unbounded string operations. If a fault
 * happens while this path is active, panic_guard_enter() rejects it and the
 * CPU goes directly to the emergency cli/hlt loop with no further output.
 */
#include "kernel.h"

#if defined(ARC_PANIC_HOST_TEST)
#define PANIC_CLI() do { } while (0)
#else
#define PANIC_CLI() __asm__ volatile("cli" ::: "memory")
#endif

/* Single-CPU state; the entry point disables interrupts before touching it. */
static volatile int panic_state;

#if defined(ARC_PANIC_DIAG_TEST)
static volatile int panic_recursive_test_armed;
#endif

/* Index 32 is the shared in-range fallback for corrupt/out-of-range vectors. */
static const char* panic_exception_names[33] = {
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
    "#SX security exception",  "reserved (31)",
    "unknown vector"
};

int panic_guard_enter(void) {
    if (panic_state) return 0;
    panic_state = 1;
    return 1;
}

int panic_guard_active(void) {
    return panic_state != 0;
}

#if defined(ARC_PANIC_HOST_TEST)
void panic_guard_reset_for_test(void) {
    panic_state = 0;
}
#endif

#if defined(ARC_PANIC_DIAG_TEST)
void panic_test_arm_recursive(void) {
    PANIC_CLI();
    panic_recursive_test_armed = 1;
}
#endif

/*
 * Every emitter reserves the final byte of the caller's buffer. A tiny
 * buffer therefore truncates safely and never clobbers the last sentinel.
 */
static void panic_buf_init(char* buf, size_t cap) {
    if (buf && cap >= 2) buf[0] = '\0';
}

static size_t panic_emit(char* buf, size_t cap, size_t pos, char c) {
    if (!buf || cap < 3 || pos >= cap - 2) return pos;
    buf[pos++] = c;
    buf[pos] = '\0';
    return pos;
}

static size_t panic_append_cstr(char* buf, size_t cap, size_t pos,
                                const char* s, size_t limit) {
    if (!s) return pos;
    for (size_t i = 0; i < limit && s[i]; i++)
        pos = panic_emit(buf, cap, pos, s[i]);
    return pos;
}

static size_t panic_append_hex64(char* buf, size_t cap, size_t pos,
                                 uint64_t v) {
    static const char hex[] = "0123456789abcdef";
    pos = panic_emit(buf, cap, pos, '0');
    pos = panic_emit(buf, cap, pos, 'x');
    for (int i = 15; i >= 0; i--)
        pos = panic_emit(buf, cap, pos, hex[(v >> (i * 4)) & 0xFULL]);
    return pos;
}

static size_t panic_append_dec64(char* buf, size_t cap, size_t pos,
                                 uint64_t v) {
    char tmp[20];
    size_t n = 0;
    if (v == 0) return panic_emit(buf, cap, pos, '0');
    while (v > 0 && n < sizeof(tmp)) {
        tmp[n++] = (char)('0' + (v % 10ULL));
        v /= 10ULL;
    }
    while (n > 0) pos = panic_emit(buf, cap, pos, tmp[--n]);
    return pos;
}

size_t panic_format_kv(char* buf, size_t cap, const char* key, uint64_t value) {
    size_t pos = 0;
    panic_buf_init(buf, cap);
    pos = panic_append_cstr(buf, cap, pos, key, 32u);
    pos = panic_emit(buf, cap, pos, '=');
    pos = panic_append_hex64(buf, cap, pos, value);
    pos = panic_emit(buf, cap, pos, '\n');
    return pos;
}

void panic_decode_pf_error(uint64_t err, unsigned features,
                           char* buf, size_t cap) {
    size_t pos = 0;
    panic_buf_init(buf, cap);

    pos = panic_append_cstr(buf, cap, pos, "PF error=", 16u);
    pos = panic_append_hex64(buf, cap, pos, err);

    pos = panic_append_cstr(buf, cap, pos, " P=", 3u);
    pos = panic_append_cstr(buf, cap, pos,
                            (err & 1u) ? "1 protection-violation"
                                       : "0 not-present", 24u);

    pos = panic_append_cstr(buf, cap, pos, " W=", 3u);
    pos = panic_append_cstr(buf, cap, pos,
                            (err & 2u) ? "1 write" : "0 read", 8u);

    pos = panic_append_cstr(buf, cap, pos, " U=", 3u);
    pos = panic_append_cstr(buf, cap, pos,
                            (err & 4u) ? "1 user" : "0 supervisor", 13u);

    pos = panic_append_cstr(buf, cap, pos, " RSVD=", 6u);
    pos = panic_append_cstr(buf, cap, pos,
                            (err & 8u) ? "1 reserved-bit-violation" : "0", 24u);

    pos = panic_append_cstr(buf, cap, pos, " I/D=", 5u);
    pos = panic_append_cstr(buf, cap, pos,
                            (err & 16u) ? "1 instruction-fetch"
                                        : "0 data-access", 19u);

    pos = panic_append_cstr(buf, cap, pos, " PK=", 4u);
    if (!(err & 32u)) {
        pos = panic_append_cstr(buf, cap, pos, "0", 1u);
    } else if (features & PANIC_PF_PKE) {
        pos = panic_append_cstr(buf, cap, pos, "1 protection-key-violation", 27u);
    } else {
        pos = panic_append_cstr(buf, cap, pos, "1 feature-not-enabled", 22u);
    }

    pos = panic_append_cstr(buf, cap, pos, " SS=", 4u);
    if (!(err & 64u)) {
        pos = panic_append_cstr(buf, cap, pos, "0", 1u);
    } else if (features & PANIC_PF_CET_SS) {
        pos = panic_append_cstr(buf, cap, pos, "1 shadow-stack-access", 22u);
    } else {
        pos = panic_append_cstr(buf, cap, pos, "1 feature-not-enabled", 22u);
    }

    pos = panic_append_cstr(buf, cap, pos, " SGX=", 5u);
    if (!(err & 0x8000u)) {
        pos = panic_append_cstr(buf, cap, pos, "0", 1u);
    } else if (features & PANIC_PF_SGX) {
        pos = panic_append_cstr(buf, cap, pos, "1 sgx-fault", 12u);
    } else {
        pos = panic_append_cstr(buf, cap, pos, "1 feature-not-enabled", 22u);
    }
    pos = panic_emit(buf, cap, pos, '\n');
    (void)pos;
}

/*
 * Vector 8 proves the CPU delivered through IST1, but it says nothing about
 * where the saved frame actually lives. Only a frame address inside the
 * dedicated double-fault stack can certify the IST claim in the report.
 */
int panic_df_frame_on_ist_stack(const exception_frame_t* frame) {
    if (!frame) return 0;
    if ((frame->vec & 0xFFULL) != DOUBLE_FAULT_VECTOR) return 0;
    return frame_on_double_fault_stack((uint64_t)(uintptr_t)frame) != 0;
}

void panic_format_stack_state(const exception_frame_t* frame,
                              uint64_t handler_rsp, int ist,
                              char* buf, size_t cap) {
    size_t pos = 0;
    panic_buf_init(buf, cap);
    if (!frame) {
        pos = panic_append_cstr(buf, cap, pos, "stack state unavailable", 23u);
        pos = panic_emit(buf, cap, pos, '\n');
        (void)pos;
        return;
    }

    int user = (frame->cs & 3ULL) == 3ULL;
    pos = panic_append_cstr(buf, cap, pos, "interrupted ", 12u);
    if (user || ist) {
        /* Ring-3 privilege changes and IST entries both push old SS:RSP. */
        pos = panic_append_cstr(buf, cap, pos, "rsp=", 4u);
        pos = panic_append_hex64(buf, cap, pos, frame->rsp);
        pos = panic_append_cstr(buf, cap, pos, " interrupted ss=", 16u);
        pos = panic_append_hex64(buf, cap, pos, frame->ss);
    } else {
        pos = panic_append_cstr(buf, cap, pos,
                                "rsp/ss unavailable (ring 0, no IST)", 35u);
    }

    pos = panic_append_cstr(buf, cap, pos, " handler stack=", 15u);
    pos = panic_append_hex64(buf, cap, pos, handler_rsp);
    /* `ist` decodes CPU-pushed SS:RSP; only a verified frame labels IST. */
    if (panic_df_frame_on_ist_stack(frame)) {
        pos = panic_append_cstr(buf, cap, pos, " (IST)", 6u);
    } else if (user) {
        pos = panic_append_cstr(buf, cap, pos, " (privilege-transition)", 23u);
    } else {
        pos = panic_append_cstr(buf, cap, pos, " (current RSP)", 14u);
    }
    pos = panic_emit(buf, cap, pos, '\n');
    (void)pos;
}

int panic_vector_needs_cr2(uint64_t vec) {
    return vec == 14u;
}

const char* panic_exception_name(uint64_t vec) {
    uint32_t v = (uint32_t)(vec & 0xFFu);
    return panic_exception_names[v < 32u ? v : 32u];
}

PANIC_NORETURN void panic_emergency_halt(void) {
#if defined(ARC_PANIC_HOST_TEST)
    for (;;) { }
#else
    for (;;) __asm__ volatile("cli; hlt");
#endif
}

static void panic_write_bounded(const char* s, size_t limit) {
    size_t len = 0;
    if (!s) return;
    while (len < limit && s[len]) len++;
    serial_write(s, (uint32_t)len);
}

#define PANIC_PUTS(lit) panic_write_bounded((lit), sizeof(lit) - 1u)

static void panic_write_line(const char* line) {
    panic_write_bounded(line, PANIC_LINE_MAX);
}

static uint64_t panic_current_rsp(void) {
#if defined(ARC_PANIC_HOST_TEST)
    return 0;
#else
    uint64_t rsp;
    __asm__ volatile("movq %%rsp, %0" : "=r"(rsp));
    return rsp;
#endif
}

static uint64_t panic_read_cr2(void) {
#if defined(ARC_PANIC_HOST_TEST)
    return 0;
#else
    uint64_t cr2;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    return cr2;
#endif
}

static unsigned panic_pf_features(void) {
#if defined(ARC_PANIC_HOST_TEST)
    return 0;
#else
    unsigned features = 0;
    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    if (cr4 & (1ULL << 22)) features |= PANIC_PF_PKE;
    if (cr4 & (1ULL << 23)) features |= PANIC_PF_CET_SS;

    uint32_t max_leaf, eax, ebx, ecx, edx;
    __asm__ volatile("cpuid"
                     : "=a"(max_leaf), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(0u), "c"(0u));
    if (max_leaf >= 7u) {
        __asm__ volatile("cpuid"
                         : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                         : "a"(7u), "c"(0u));
        if (ebx & (1u << 2)) features |= PANIC_PF_SGX;
    }
    return features;
#endif
}

static void panic_write_pid(char* line, size_t cap) {
    int pid = current_process;
    size_t pos = 0;
    panic_buf_init(line, cap);
    if (pid >= 0 && pid < MAX_PROCESSES) {
        pos = panic_append_cstr(line, cap, pos, "pid=", 4u);
        pos = panic_append_dec64(line, cap, pos, (uint64_t)pid);
        pos = panic_append_cstr(line, cap, pos, " (index; state not trusted)",
                                27u);
    } else {
        pos = panic_append_cstr(line, cap, pos,
                                "pid unavailable (state not trusted)", 35u);
    }
    pos = panic_emit(line, cap, pos, '\n');
    (void)pos;
    panic_write_line(line);
}

PANIC_NORETURN void kernel_panic(const char* reason) {
    PANIC_CLI();
    if (!panic_guard_enter()) panic_emergency_halt();

    char line[PANIC_LINE_MAX];
    PANIC_PUTS("=== KERNEL PANIC ===\n");
    panic_buf_init(line, sizeof(line));
    size_t pos = panic_append_cstr(line, sizeof(line), 0, "reason: ", 8u);
    pos = panic_append_cstr(line, sizeof(line), pos, reason, 128u);
    pos = panic_emit(line, sizeof(line), pos, '\n');
    (void)pos;
    panic_write_line(line);
    PANIC_PUTS("System halted\n");
    panic_emergency_halt();
}

PANIC_NORETURN void kernel_panic_exception(const exception_frame_t* frame) {
    PANIC_CLI();
    if (!panic_guard_enter()) panic_emergency_halt();
    if (!frame) kernel_panic("CPU exception with null frame");

    char line[PANIC_LINE_MAX];
    uint64_t vec = frame->vec & 0xFFULL;
    uint32_t v = (uint32_t)vec;
    int ist = (v == DOUBLE_FAULT_VECTOR);
    uint64_t handler_rsp = panic_current_rsp();

    PANIC_PUTS("=== KERNEL PANIC ===\n");
    PANIC_PUTS("=== KERNEL EXCEPTION ===\n");

    panic_buf_init(line, sizeof(line));
    size_t pos = panic_append_cstr(line, sizeof(line), 0, "Exception: ", 11u);
    pos = panic_append_cstr(line, sizeof(line), pos,
                            panic_exception_name(vec), 48u);
    pos = panic_append_cstr(line, sizeof(line), pos, "  vector=", 9u);
    pos = panic_append_dec64(line, sizeof(line), pos, vec);
    pos = panic_append_cstr(line, sizeof(line), pos, "  error=", 8u);
    pos = panic_append_hex64(line, sizeof(line), pos, frame->err);
    pos = panic_emit(line, sizeof(line), pos, '\n');
    (void)pos;
    panic_write_line(line);

    PANIC_PUTS("Mode: kernel\n");
    panic_write_pid(line, sizeof(line));

    panic_format_stack_state(frame, handler_rsp, ist, line, sizeof(line));
    panic_write_line(line);

    panic_format_kv(line, sizeof(line), "rip", frame->rip);
    panic_write_line(line);
    panic_format_kv(line, sizeof(line), "cs", frame->cs);
    panic_write_line(line);
    panic_format_kv(line, sizeof(line), "rflags", frame->rflags);
    panic_write_line(line);

#if defined(ARC_DOUBLE_FAULT_TEST)
    if (v == DOUBLE_FAULT_VECTOR) {
        if (panic_df_frame_on_ist_stack(frame)) {
            PANIC_PUTS("DOUBLEFAULT frame on dedicated IST stack\n");
        } else {
            PANIC_PUTS("DOUBLEFAULT frame NOT on dedicated IST stack\n");
        }
    }
#endif

    if (panic_vector_needs_cr2(vec)) {
        uint64_t cr2 = panic_read_cr2();
        panic_format_kv(line, sizeof(line), "cr2 (faulting address)", cr2);
        panic_write_line(line);
        panic_decode_pf_error(frame->err, panic_pf_features(),
                              line, sizeof(line));
        panic_write_line(line);
    }

    PANIC_PUTS("Registers:\n");
    const uint64_t regs[15] = {
        frame->r15, frame->r14, frame->r13, frame->r12, frame->rbp,
        frame->rbx, frame->r11, frame->r10, frame->r9, frame->r8,
        frame->rdi, frame->rsi, frame->rdx, frame->rcx, frame->rax
    };
    static const char* reg_names[15] = {
        "r15", "r14", "r13", "r12", "rbp", "rbx", "r11",
        "r10", "r9", "r8", "rdi", "rsi", "rdx", "rcx", "rax"
    };
    for (int i = 0; i < 15; i++) {
        panic_format_kv(line, sizeof(line), reg_names[i], regs[i]);
        panic_write_line(line);
    }

    PANIC_PUTS("System halted\n");

#if defined(ARC_PANIC_DIAG_TEST)
    if (panic_recursive_test_armed) {
        panic_recursive_test_armed = 0;
        PANIC_PUTS("PANICREC first panic report complete; recursive entry follows\n");
        kernel_panic("PANICREC recursive fatal entry (must not print)");
    }
#endif
    panic_emergency_halt();
}
