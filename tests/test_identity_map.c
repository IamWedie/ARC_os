/*
 * ARC OS identity-map coverage test.
 *
 * Contract under test: every 4 KiB frame that PMM can hand out from a recorded
 * AVAILABLE region must be reachable through vmm_init()'s 2 MiB identity map.
 *
 * Links the REAL kernel/pmm.c. The identity-map set is modeled from the loop in
 * kernel/vmm.c, and the region-start expression is read out of that same source
 * file at runtime, so this test follows the production code instead of a copy.
 * If the start expression changes to something unrecognized, the test fails
 * loudly rather than silently checking a stale model.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#undef SYS_OPEN /* MinGW stdio uses this name for _SYS_OPEN */
#include "kernel.h"
#include "bootinfo.h"

/* pmm.c resolves these at link time. MinGW places them near its image base;
 * the guard in main() proves they stay clear of the physical ranges tested. */
unsigned char __kernel_start[16];
unsigned char __kernel_end[16];

#define TWO_MB        0x200000ULL
#define PAGE_2MB(pa)  ((pa) & ~(TWO_MB - 1))
#define VMM_PD_COUNT  128 /* kernel/vmm.c:16,50 */

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

/* -1 = source not found, 0 = round-up, 1 = round-down. */
static int vmm_start_is_round_down(void) {
    static const char* paths[] = { "kernel/vmm.c", "../kernel/vmm.c" };

    for (size_t p = 0; p < sizeof(paths) / sizeof(paths[0]); p++) {
        FILE* f = fopen(paths[p], "r");
        char line[512];
        int found = 0;
        int round_down = 0;

        if (!f) continue;
        while (fgets(line, sizeof line, f)) {
            char* s = strstr(line, "uint64_t start =");
            if (!s || !strstr(s, "r.base")) continue;
            found = 1;
            if (strstr(s, "r.base & ~0x1FFFFFULL")) round_down = 1;
            break;
        }
        fclose(f);
        if (found) return round_down;
    }
    return -1;
}

static int g_round_down; /* set from the real source in main() */

static uint64_t vmm_region_start(uint64_t base) {
    if (g_round_down) return base & ~(TWO_MB - 1);
    return (base + (TWO_MB - 1)) & ~(TWO_MB - 1);
}

/* Mirrors vmm.c:76-80: aligned pages a with a < claimed_end, plus the
 * vmm_map_2mb() PD-count/PML4 guard at vmm.c:45-52. */
static int region_loop_covers(uint64_t base, uint64_t len, uint64_t page) {
    uint64_t claimed_end = base + len;
    uint64_t start = vmm_region_start(base);
    uint64_t pdpti = (page >> 30) & 0x1FF;
    uint64_t pml4i = (page >> 39) & 0x1FF;

    if (pml4i != 0 || pdpti >= VMM_PD_COUNT) return 0;
    for (uint64_t a = start; a < claimed_end && a >= start; a += TWO_MB)
        if (a == page) return 1;
    return 0;
}

/* Mirrors the whole identity map: vmm.c:60's unconditional first 2 MiB plus
 * the per-region loop. */
static int identity_covers(uint64_t base, uint64_t len, uint64_t pa) {
    uint64_t page = PAGE_2MB(pa);
    if (page == 0) return 1;
    return region_loop_covers(base, len, page);
}

static struct bootinfo make_map(uint64_t base, uint64_t len) {
    struct bootinfo bi;
    memset(&bi, 0, sizeof bi);
    bi.flags = BOOTINFO_FLAG_MMAP;
    bi.mmap_count = 1;
    bi.mmap[0].base = base;
    bi.mmap[0].len = len;
    bi.mmap[0].type = BOOTINFO_MMAP_AVAILABLE;
    return bi;
}

/* For one contiguous run returned by the REAL allocator, require every frame
 * to be covered. Reports only the first uncovered physical address. */
static void check_allocated_run(uint64_t base, uint64_t len,
                                void* run, size_t frames, const char* label) {
    uint64_t missing = 0;
    uint64_t first_missing = 0;

    CHECK(run != NULL, "%s: PMM returned a run", label);
    if (!run) return;
    CHECK((uint64_t)run == base, "%s: run starts at region base", label);

    for (size_t j = 0; j < frames; j++) {
        uint64_t pa = (uint64_t)run + (uint64_t)j * 4096ULL;
        if (!identity_covers(base, len, pa)) {
            missing++;
            if (!first_missing) first_missing = pa;
        }
    }
    CHECK(missing == 0,
          "%s: %u of %u frames unmapped; first PA 0x%llX",
          label, (unsigned)missing, (unsigned)frames,
          (unsigned long long)first_missing);
}

static void test_arbitrary_region_bases(void) {
    static const uint64_t fixed_bases[] = {
        0x00100000ULL, /* real-map head: covered by the first-page rule either way */
        0x00101000ULL, 0x00200000ULL, 0x00201000ULL,
        0x00300000ULL, 0x003FF000ULL, 0x00400000ULL, 0x00405000ULL,
        0x00800000ULL, 0x00FFF000ULL, 0x01000000ULL, 0x01001000ULL,
    };
    const uint64_t len = 6 * TWO_MB;
    const size_t frames = (size_t)(len >> 12);
    uint64_t rng = 0x12345678ULL;
    char label[96];

    for (size_t i = 0; i < sizeof(fixed_bases) / sizeof(fixed_bases[0]); i++) {
        struct bootinfo bi = make_map(fixed_bases[i], len);
        snprintf(label, sizeof label, "fixed base 0x%llX",
                 (unsigned long long)fixed_bases[i]);
        pmm_init(&bi);
        check_allocated_run(fixed_bases[i], len,
                            pmm_alloc_pages(frames), frames, label);
    }

    /* Deterministic arbitrary 4 KiB-aligned bases across [1 MiB, 65 MiB). */
    for (int i = 0; i < 96; i++) {
        uint64_t index;
        uint64_t base;

        rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
        index = (rng >> 32) % (64ULL * 1024ULL * 1024ULL / 4096ULL);
        base = 0x100000ULL + index * 4096ULL;

        {
            struct bootinfo bi = make_map(base, len);
            snprintf(label, sizeof label, "random base 0x%llX",
                     (unsigned long long)base);
            pmm_init(&bi);
            check_allocated_run(base, len, pmm_alloc_pages(frames), frames, label);
        }
    }
}

/* Captured from the real -m 512M QEMU gate (64-bit decimal, serial output).
 * Entry 6 is the >4 GiB reserved block recorded in tasks/todo.md; PMM must not
 * allocate it, so the fixed PDE mask is out of scope for this test. */
static void test_real_qemu_map(void) {
    static const uint64_t bases[7] = {
        0ULL, 654336ULL, 983040ULL, 1048576ULL,
        536739840ULL, 4294705152ULL, 1086626725888ULL,
    };
    static const uint64_t lens[7] = {
        654336ULL, 1024ULL, 65536ULL, 535691264ULL,
        131072ULL, 262144ULL, 12884901888ULL,
    };
    static const uint32_t types[7] = { 1, 2, 2, 1, 2, 2, 2 };
    const uint64_t ram_base = 1048576ULL;
    const uint64_t ram_len = 535691264ULL;
    struct bootinfo bi;
    void* run;

    memset(&bi, 0, sizeof bi);
    bi.flags = BOOTINFO_FLAG_MMAP;
    bi.mmap_count = 7;
    for (int i = 0; i < 7; i++) {
        bi.mmap[i].base = bases[i];
        bi.mmap[i].len = lens[i];
        bi.mmap[i].type = types[i];
    }

    pmm_init(&bi);
    CHECK(pmm_region_count_get() == 2, "real map: two AVAILABLE regions recorded");

    run = pmm_alloc_pages(512);
    check_allocated_run(ram_base, ram_len, run, 512, "real map run");

    /* The head of the real region is covered only by vmm.c:60; the interior
     * and tail come from the region loop. Keep these as explicit controls. */
    CHECK(identity_covers(ram_base, ram_len, ram_base),
          "real map: region head 0x100000 covered");
    CHECK(identity_covers(ram_base, ram_len, 0x300000ULL),
          "real map: interior frame 0x300000 covered");
    CHECK(identity_covers(ram_base, ram_len, 0x1FFD0000ULL),
          "real map: tail frame 0x1FFD0000 covered");
}

int main(void) {
    uint64_t stub_lo = (uint64_t)__kernel_start;
    uint64_t stub_hi = (uint64_t)__kernel_end;

    printf("ARC OS identity-map coverage test\n");

    CHECK(stub_hi <= 0x100000ULL || stub_lo >= 0x8000000ULL,
          "host kernel-image stub stays out of tested physical ranges");

    g_round_down = vmm_start_is_round_down();
    CHECK(g_round_down == 1,
          "kernel/vmm.c region start rounds DOWN (r.base & ~0x1FFFFFULL)");

    test_arbitrary_region_bases();
    test_real_qemu_map();

    printf("\nIdentity-map test summary: %d passed, %d failed\n",
           tests_passed, tests_failed);
    return tests_failed ? 1 : 0;
}
