/*
 * ARC OS kernel panic / CPU fault diagnostics host test.
 *
 * Links the REAL kernel/panic.c with hardware output stubbed out. It proves
 * the panic guard rejects recursive entry, formatting is bounded and
 * allocation-free, page-fault decode is gated by architectural feature
 * support, interrupted-stack availability is derived from the saved frame,
 * CR2 is limited to #PF, and the exception-frame ABI is unchanged.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#undef SYS_OPEN /* MinGW stdio uses this name for _SYS_OPEN */
#include "kernel.h"

volatile int current_process = 1;
volatile int num_processes = 1;
volatile int preempt_disabled = 0;
process_t processes[MAX_PROCESSES];

/* Controllable bounds for the real frame_on_double_fault_stack() contract. */
static uint64_t host_df_base;
static uint64_t host_df_top;

int frame_on_double_fault_stack(uint64_t frame_rsp) {
    return frame_rsp >= host_df_base && frame_rsp < host_df_top;
}

void serial_write(const char* s, uint32_t len) {
    (void)s;
    (void)len;
}

void scheduler_exit_process(int pid, int code) {
    (void)pid;
    (void)code;
}

uint64_t scheduler_switch(uint64_t rsp) {
    return rsp;
}

static int tests_passed;
static int tests_failed;

#define CHECK(cond, ...)                                      \
    do {                                                      \
        if (cond) {                                           \
            tests_passed++;                                   \
        } else {                                              \
            tests_failed++;                                   \
            printf("  FAIL: ");                               \
            printf(__VA_ARGS__);                              \
            printf("\n");                                     \
        }                                                     \
    } while (0)

#define HAS(text, needle) (strstr((text), (needle)) != NULL)

static void test_panic_guard(void) {
    printf("\n=== Test 1: panic guard allows one entry ===\n");

    panic_guard_reset_for_test();
    CHECK(!panic_guard_active(), "panic state active before first entry");
    CHECK(panic_guard_enter() == 1, "first panic entry was rejected");
    CHECK(panic_guard_active(), "panic state not active after first entry");
    CHECK(panic_guard_enter() == 0, "recursive panic entry was accepted");

    panic_guard_reset_for_test();
    CHECK(!panic_guard_active(), "panic state did not reset for next case");
    CHECK(panic_guard_enter() == 1, "fresh panic entry was rejected");
}

static void test_bounded_formatting(void) {
    printf("\n=== Test 2: bounded allocation-free formatting ===\n");

    char line[PANIC_LINE_MAX];
    memset(line, 'Z', sizeof(line));
    size_t n = panic_format_kv(line, sizeof(line), "rip", 0x1234ULL);
    CHECK(n > 0 && n < sizeof(line), "formatted length %u out of bounds",
          (unsigned)n);
    CHECK(HAS(line, "rip=0x0000000000001234"),
          "formatted line missing bounded hex value: %s", line);
    CHECK(line[sizeof(line) - 1] == 'Z',
          "formatting wrote past the requested buffer");

    char tiny[4];
    memset(tiny, 'Z', sizeof(tiny));
    n = panic_format_kv(tiny, sizeof(tiny), "rip", 0x1234ULL);
    CHECK(n <= sizeof(tiny) - 1, "tiny buffer length %u is unbounded",
          (unsigned)n);
    CHECK(tiny[3] == 'Z', "tiny buffer sentinel was overwritten");

    CHECK(panic_format_kv(NULL, 0, "rip", 1) == 0,
          "zero-capacity formatting did not fail safely");
}

static void test_pf_decode(void) {
    printf("\n=== Test 3: page-fault error-code decode ===\n");

    char line[PANIC_LINE_MAX];

    panic_decode_pf_error(0x0ULL, 0u, line, sizeof(line));
    CHECK(HAS(line, "P=0 not-present"), "P=0 not decoded: %s", line);
    CHECK(HAS(line, "W=0 read"), "W=0 not decoded: %s", line);
    CHECK(HAS(line, "U=0 supervisor"), "U=0 not decoded: %s", line);

    panic_decode_pf_error(0x7ULL | 0x8ULL | 0x10ULL, 0u, line, sizeof(line));
    CHECK(HAS(line, "P=1 protection-violation"), "P=1 not decoded: %s", line);
    CHECK(HAS(line, "W=1 write"), "W=1 not decoded: %s", line);
    CHECK(HAS(line, "U=1 user"), "U=1 not decoded: %s", line);
    CHECK(HAS(line, "RSVD=1 reserved-bit-violation"),
          "RSVD=1 not decoded: %s", line);
    CHECK(HAS(line, "I/D=1 instruction-fetch"),
          "I/D=1 not decoded: %s", line);

    panic_decode_pf_error(0x20ULL, 0u, line, sizeof(line));
    CHECK(HAS(line, "PK=1 feature-not-enabled"),
          "unsupported PK bit was claimed as supported: %s", line);
    CHECK(!HAS(line, "protection-key-violation"),
          "unsupported PK bit decoded as enabled feature: %s", line);

    panic_decode_pf_error(0x20ULL, PANIC_PF_PKE, line, sizeof(line));
    CHECK(HAS(line, "PK=1 protection-key-violation"),
          "supported PK bit not decoded: %s", line);

    panic_decode_pf_error(0x40ULL, 0u, line, sizeof(line));
    CHECK(HAS(line, "SS=1 feature-not-enabled"),
          "unsupported SS bit was claimed as supported: %s", line);
    panic_decode_pf_error(0x40ULL, PANIC_PF_CET_SS, line, sizeof(line));
    CHECK(HAS(line, "SS=1 shadow-stack-access"),
          "supported SS bit not decoded: %s", line);

    panic_decode_pf_error(0x8000ULL, 0u, line, sizeof(line));
    CHECK(HAS(line, "SGX=1 feature-not-enabled"),
          "unsupported SGX bit was claimed as supported: %s", line);
    panic_decode_pf_error(0x8000ULL, PANIC_PF_SGX, line, sizeof(line));
    CHECK(HAS(line, "SGX=1 sgx-fault"),
          "supported SGX bit not decoded: %s", line);

    memset(line, 'Z', sizeof(line));
    panic_decode_pf_error(1u, 0u, line, 8u);
    CHECK(line[7] == 'Z', "page-fault decoder wrote past a tiny buffer");
}

static void test_stack_state(void) {
    printf("\n=== Test 4: interrupted-stack availability ===\n");

    exception_frame_t f;
    memset(&f, 0, sizeof(f));
    char line[PANIC_LINE_MAX];

    f.cs = 0x23;
    f.rsp = 0x9000000000ULL;
    f.ss = 0x1B;
    panic_format_stack_state(&f, 0x20000000ULL, 0, line, sizeof(line));
    CHECK(HAS(line, "interrupted rsp=0x0000009000000000"),
          "ring-3 interrupted RSP not reported: %s", line);
    CHECK(HAS(line, "interrupted ss=0x000000000000001b"),
          "ring-3 interrupted SS not reported: %s", line);
    CHECK(HAS(line, "handler stack=0x0000000020000000 (privilege-transition)"),
          "ring-3 handler stack not distinguished: %s", line);

    f.cs = 0x08;
    panic_format_stack_state(&f, 0x20000000ULL, 0, line, sizeof(line));
    CHECK(HAS(line, "interrupted rsp/ss unavailable (ring 0, no IST)"),
          "ring-0 non-IST availability not explicit: %s", line);
    CHECK(!HAS(line, "interrupted rsp=0x"),
          "fabricated interrupted RSP printed for ring-0 non-IST frame: %s",
          line);
    CHECK(HAS(line, "handler stack=0x0000000020000000 (current RSP)"),
          "ring-0 handler stack not labelled: %s", line);

    f.rsp = 0x000000000023ef50ULL;
    f.ss = 0;
    f.vec = DOUBLE_FAULT_VECTOR;
    host_df_base = (uint64_t)(uintptr_t)&f;
    host_df_top = host_df_base + sizeof(f);
    panic_format_stack_state(&f, 0x20000000ULL, 1, line, sizeof(line));
    CHECK(HAS(line, "interrupted rsp=0x000000000023ef50"),
          "IST interrupted RSP not reported: %s", line);
    CHECK(HAS(line, "handler stack=0x0000000020000000 (IST)"),
          "IST handler stack not distinguished: %s", line);
    CHECK(!HAS(line, "unavailable"),
          "IST frame incorrectly marked unavailable: %s", line);
}

static void test_cr2_policy(void) {
    printf("\n=== Test 5: CR2 is page-fault only ===\n");

    CHECK(panic_vector_needs_cr2(14) == 1, "#PF vector 14 does not require CR2");
    for (uint64_t v = 0; v < 32; v++) {
        if (v == 14) continue;
        CHECK(panic_vector_needs_cr2(v) == 0,
              "vector %llu incorrectly requires CR2",
              (unsigned long long)v);
    }
    CHECK(HAS(panic_exception_name(8), "#DF double fault"),
          "vector 8 name is not #DF");
    CHECK(HAS(panic_exception_name(99), "unknown vector"),
          "out-of-range vector has no fallback name");

    exception_frame_t df;
    memset(&df, 0, sizeof(df));
    df.cs = 0x23;  /* ring 3, but #DF is never user-isolated */
    df.vec = DOUBLE_FAULT_VECTOR;
    CHECK(exception_frame_is_fatal(&df) == 1,
          "ring-3 #DF must take the kernel-fatal path");

    exception_frame_t user_ud;
    memset(&user_ud, 0, sizeof(user_ud));
    user_ud.cs = 0x23;
    user_ud.vec = 6;
    CHECK(exception_frame_is_fatal(&user_ud) == 0,
          "ordinary ring-3 #UD must stay on the user-isolation path");
}

static void test_exception_frame_abi(void) {
    printf("\n=== Test 6: exception-frame ABI unchanged ===\n");

    CHECK(sizeof(exception_frame_t) == 176,
          "frame size = %u, want 176", (unsigned)sizeof(exception_frame_t));
    CHECK(offsetof(exception_frame_t, vec) == 120,
          "vec offset = %u, want 120", (unsigned)offsetof(exception_frame_t, vec));
    CHECK(offsetof(exception_frame_t, err) == 128,
          "err offset = %u, want 128", (unsigned)offsetof(exception_frame_t, err));
    CHECK(offsetof(exception_frame_t, rip) == 136,
          "rip offset = %u, want 136", (unsigned)offsetof(exception_frame_t, rip));
    CHECK(offsetof(exception_frame_t, cs) == 144,
          "cs offset = %u, want 144", (unsigned)offsetof(exception_frame_t, cs));
    CHECK(offsetof(exception_frame_t, rflags) == 152,
          "rflags offset = %u, want 152",
          (unsigned)offsetof(exception_frame_t, rflags));
    CHECK(offsetof(exception_frame_t, rsp) == 160,
          "rsp offset = %u, want 160", (unsigned)offsetof(exception_frame_t, rsp));
    CHECK(offsetof(exception_frame_t, ss) == 168,
          "ss offset = %u, want 168", (unsigned)offsetof(exception_frame_t, ss));
}

static void test_df_frame_bounds(void) {
    printf("\n=== Test 7: double-fault frame must be bounds-checked ===\n");

    exception_frame_t f;
    memset(&f, 0, sizeof(f));
    f.cs = 0x08;
    f.vec = DOUBLE_FAULT_VECTOR;
    char line[PANIC_LINE_MAX];

    /*
     * Regression: vector 8 alone must not certify the frame. With bounds that
     * exclude this frame, the CPU-pushed interrupted RSP/SS are still decoded
     * (IST entry), but the handler stack must not be labelled verified IST.
     */
    host_df_base = 0x1000ULL;
    host_df_top = 0x2000ULL;
    CHECK(panic_df_frame_on_ist_stack(&f) == 0,
          "vector-8 frame outside the DF stack was verified");
    CHECK(panic_df_frame_on_ist_stack(NULL) == 0,
          "null frame was verified as a DF IST frame");
    panic_format_stack_state(&f, 0x3000ULL, 1, line, sizeof(line));
    CHECK(HAS(line, "interrupted rsp=0x0000000000000000"),
          "vector-8 IST interrupted RSP/SS interpretation was lost: %s", line);
    CHECK(!HAS(line, "handler stack=0x0000000000003000 (IST)"),
          "out-of-bounds #DF frame was certified as IST from vector only: %s",
          line);
    CHECK(HAS(line, "handler stack=0x0000000000003000 (current RSP)"),
          "out-of-bounds #DF handler stack lost a truthful label: %s", line);

    /* The same frame becomes verified only when its address is in bounds. */
    host_df_base = (uint64_t)(uintptr_t)&f;
    host_df_top = host_df_base + sizeof(f);
    CHECK(panic_df_frame_on_ist_stack(&f) == 1,
          "in-bounds vector-8 frame was not verified");
    panic_format_stack_state(&f, 0x3000ULL, 1, line, sizeof(line));
    CHECK(HAS(line, "handler stack=0x0000000000003000 (IST)"),
          "in-bounds #DF frame was not certified as IST: %s", line);

    /* Being in bounds is not enough for a different vector. */
    f.vec = 6;
    CHECK(panic_df_frame_on_ist_stack(&f) == 0,
          "non-#DF frame inside DF-stack bounds was verified");
    panic_format_stack_state(&f, 0x3000ULL, 1, line, sizeof(line));
    CHECK(!HAS(line, "handler stack=0x0000000000003000 (IST)"),
          "non-#DF frame was certified as IST: %s", line);
}

int main(void) {
    printf("=== ARC OS panic diagnostics host test ===\n");
    test_panic_guard();
    test_bounded_formatting();
    test_pf_decode();
    test_stack_state();
    test_cr2_policy();
    test_exception_frame_abi();
    test_df_frame_bounds();

    printf("\n%d checks passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed ? 1 : 0;
}
