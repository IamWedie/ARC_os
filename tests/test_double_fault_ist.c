/*
 * ARC OS dedicated double-fault IST host test.
 *
 * Links the REAL kernel/gdt.c and kernel/idt.c with no privileged instruction
 * execution. It proves that the compile-time double-fault stack is aligned and
 * bounded, that TSS IST1 points at its top, that vector 8 selects IST1 while
 * other exception gates do not, and that the exception-frame ABI is unchanged.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#undef SYS_OPEN /* MinGW stdio uses this name for _SYS_OPEN */
#include "kernel.h"

/* Stand-in for the linker-provided normal ring-0 stack top. */
uint8_t __stack_top[16];

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

static void test_stack_bounds(void) {
    printf("\n=== Test 1: double-fault stack is aligned and bounded ===\n");

    CHECK(DOUBLE_FAULT_IST_INDEX == 1,
          "IST index = %u, want 1", (unsigned)DOUBLE_FAULT_IST_INDEX);
    CHECK(DOUBLE_FAULT_STACK_SIZE == 32u * 1024u,
          "stack size = %u, want 32768", (unsigned)DOUBLE_FAULT_STACK_SIZE);

    uint64_t base = double_fault_stack_base();
    uint64_t top = double_fault_stack_top();

    CHECK((base & 4095ULL) == 0,
          "stack base 0x%llx is not 4 KiB aligned",
          (unsigned long long)base);
    CHECK((top & 15ULL) == 0,
          "stack top 0x%llx is not 16-byte aligned",
          (unsigned long long)top);
    CHECK(top == base + DOUBLE_FAULT_STACK_SIZE,
          "stack bounds 0x%llx..0x%llx do not match size",
          (unsigned long long)base, (unsigned long long)top);
    CHECK(top > base, "stack top is not above stack base");
    CHECK(top != (uint64_t)(uintptr_t)__stack_top,
          "double-fault stack reuses the normal __stack_top");
}

static void test_tss_ist1(void) {
    printf("\n=== Test 2: TSS IST1 points to the dedicated stack top ===\n");

    double_fault_ist_init();

    uint64_t base = double_fault_stack_base();
    uint64_t top = double_fault_stack_top();
    uint64_t ist1 = tss_ist1_top();

    CHECK(ist1 == top,
          "TSS IST1 = 0x%llx, want stack top 0x%llx",
          (unsigned long long)ist1, (unsigned long long)top);
    CHECK(frame_on_double_fault_stack(top - sizeof(exception_frame_t)),
          "a frame at top-sizeof(exception_frame_t) is outside IST1 bounds");
    CHECK(!frame_on_double_fault_stack(base - 8),
          "address below the dedicated stack is reported inside IST1 bounds");
    CHECK(!frame_on_double_fault_stack(top),
          "the initial RSP value itself is reported inside IST1 bounds");
}

static void test_idt_gates(void) {
    printf("\n=== Test 3: vector 8 selects IST1 without changing the gate ===\n");

    for (int i = 0; i < 32; i++) {
        uint64_t base = 0x200000ULL + (uint64_t)i * 0x10ULL;
        idt_set_exception_gate((uint8_t)i, base);
        CHECK(idt_gate_ist((uint8_t)i) ==
              (i == DOUBLE_FAULT_VECTOR ? DOUBLE_FAULT_IST_INDEX : 0),
              "vector %d IST = %u, want %u", i,
              (unsigned)idt_gate_ist((uint8_t)i),
              (unsigned)(i == DOUBLE_FAULT_VECTOR
                         ? DOUBLE_FAULT_IST_INDEX : 0));
        CHECK(idt_gate_selector((uint8_t)i) == GDTSEL_KC,
              "vector %d selector = 0x%x, want 0x%x", i,
              (unsigned)idt_gate_selector((uint8_t)i), (unsigned)GDTSEL_KC);
        CHECK(idt_gate_flags((uint8_t)i) == 0x8E,
              "vector %d flags = 0x%x, want 0x8e", i,
              (unsigned)idt_gate_flags((uint8_t)i));
        CHECK(idt_gate_offset((uint8_t)i) == base,
              "vector %d offset = 0x%llx, want 0x%llx", i,
              (unsigned long long)idt_gate_offset((uint8_t)i),
              (unsigned long long)base);
    }
}

static void test_exception_frame_abi(void) {
    printf("\n=== Test 4: exception-frame ABI is unchanged ===\n");

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

int main(void) {
    test_stack_bounds();
    test_tss_ist1();
    test_idt_gates();
    test_exception_frame_abi();

    printf("\n%d checks passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed ? 1 : 0;
}
