/*
 * ARC OS Slab Allocator Stress Test
 *
 * Links against the REAL kernel/slab.c and kernel/memory.c, with a
 * host-side PMM stub (tests/pmm_stub.c) standing in for physical memory.
 *
 * Build:
 *   gcc -O2 -Wall -Wextra -std=gnu11 -I kernel \
 *       tests/test_slab_stress.c tests/pmm_stub.c \
 *       kernel/slab.c kernel/memory.c -o tests/test_slab_stress.exe
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#undef SYS_OPEN /* MinGW stdio uses this name for _SYS_OPEN */

#include "kernel.h"
#include "slab.h"

extern uint64_t pmm_stub_used_pages(void);

/* ---------------------------------------------------------------- */
/*                        Test accounting                           */
/* ---------------------------------------------------------------- */

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

/* ---------------------------------------------------------------- */
/*                          Test 1: mapping                         */
/* ---------------------------------------------------------------- */

static void test_size_class_mapping(void) {
    static const struct { uint64_t size; int cls; } cases[] = {
        {0, 0}, {1, 0}, {15, 0}, {16, 0},
        {17, 1}, {32, 1}, {33, 2}, {64, 2},
        {65, 3}, {128, 3}, {129, 4}, {256, 4},
        {257, 5}, {512, 5}, {513, 6}, {1024, 6},
        {1025, 7}, {2048, 7}, {2049, 8}, {4096, 8},
        {4097, 9}, {8192, 9}, {8193, 10}, {16384, 10},
        {16385, 11}, {32768, 11},
        {32769, -1}, {65536, -1}, {100000, -1}, {1ULL << 40, -1}
    };

    printf("\n=== Test 1: size-class mapping ===\n");
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int got = slab_size_class(cases[i].size);
        CHECK(got == cases[i].cls,
              "size %llu expected class %d, got %d",
              (unsigned long long)cases[i].size, cases[i].cls, got);
    }
    printf("  checked %zu boundaries\n", sizeof(cases) / sizeof(cases[0]));
}

/* ---------------------------------------------------------------- */
/*                       Test 2: every class                        */
/* ---------------------------------------------------------------- */

static int class_cycle(int i) {
    struct kmem_cache* c = slab_caches[i];
    CHECK(c != NULL, "size class %d has a cache", i);
    if (!c) return -1;

    uint64_t obj_size = SLAB_SIZE_CLASS(i);
    uint64_t n = (i >= 8) ? 4u : 64u;
    void** ptrs = (void**)malloc((size_t)n * sizeof(void*));
    if (!ptrs) return -1;

    for (uint64_t j = 0; j < n; j++) ptrs[j] = NULL;

    int ok = 0;
    for (uint64_t j = 0; j < n; j++) {
        ptrs[j] = kmem_cache_alloc(c);
        CHECK(ptrs[j] != NULL, "class %d alloc %llu", i, (unsigned long long)j);
        if (!ptrs[j]) { ok = -1; goto out; }

        CHECK(((uintptr_t)ptrs[j] % 16u) == 0,
              "class %d object %llu is 16-byte aligned",
              i, (unsigned long long)j);

        uint8_t* p = (uint8_t*)ptrs[j];
        uint64_t touched = obj_size < 256 ? obj_size : 256;
        for (uint64_t k = 0; k < touched; k++) {
            p[k] = (uint8_t)(0x5A ^ (uint8_t)k ^ (uint8_t)j);
        }
    }

    for (uint64_t j = 0; j < n; j++) {
        uint8_t* p = (uint8_t*)ptrs[j];
        uint64_t touched = obj_size < 256 ? obj_size : 256;
        for (uint64_t k = 0; k < touched; k++) {
            if (p[k] != (uint8_t)(0x5A ^ (uint8_t)k ^ (uint8_t)j)) {
                CHECK(0, "class %d object %llu content corrupted",
                      i, (unsigned long long)j);
                ok = -1;
                goto out;
            }
        }
        kmem_cache_free(c, ptrs[j]);
        ptrs[j] = NULL;
    }

out:
    for (uint64_t j = 0; j < n; j++) {
        if (ptrs[j]) kmem_cache_free(c, ptrs[j]);
    }
    free(ptrs);

    slab_stats_t st;
    if (slab_get_stats(i, &st) == 0) {
        CHECK(st.live_objs == 0, "class %d live_objs is 0 after free", i);
        CHECK(st.allocs == st.frees,
              "class %d alloc/free balanced (%llu/%llu)", i,
              (unsigned long long)st.frees,
              (unsigned long long)st.allocs);
        CHECK(st.bad_frees == 0, "class %d has no rejected frees", i);
    }
    return ok;
}

static void test_every_class(void) {
    printf("\n=== Test 2: allocate/free every size class ===\n");

    for (int i = 0; i < SLAB_N_SIZE_CLASSES; i++) {
        uint64_t frames_before = pmm_stub_used_pages();
        class_cycle(i);
        uint64_t frames_after = pmm_stub_used_pages();
        uint64_t slab_frames = slab_bytes_owned() / 4096u;

        CHECK(frames_after == slab_frames,
              "class %d: PMM frames (%llu) match slab-owned frames (%llu)",
              i, (unsigned long long)frames_after,
              (unsigned long long)slab_frames);
        (void)frames_before;
    }

    /* A second cycle must reuse the cached slabs, not grow the PMM. */
    uint64_t before = pmm_stub_used_pages();
    for (int i = 0; i < SLAB_N_SIZE_CLASSES; i++) class_cycle(i);
    uint64_t after = pmm_stub_used_pages();
    CHECK(before == after,
          "second allocate/free cycle reuses slabs (frames %llu -> %llu)",
          (unsigned long long)before, (unsigned long long)after);
}

/* ---------------------------------------------------------------- */
/*                      Test 3: random stress                       */
/* ---------------------------------------------------------------- */

#define MAX_LIVE   2048
#define STRESS_OPS 20000

static void*    live_ptr[MAX_LIVE];
static uint64_t live_size[MAX_LIVE];
static uint8_t  live_tag[MAX_LIVE];
static int      live_n;

static uint64_t fill_len(uint64_t size) {
    return size < 512 ? size : 512;
}

static int ranges_overlap(uintptr_t a, uint64_t asz,
                          uintptr_t b, uint64_t bsz) {
    return a < b + bsz && b < a + asz;
}

static void stress_free_one(int idx) {
    uint8_t* p = (uint8_t*)live_ptr[idx];
    uint64_t n = fill_len(live_size[idx]);
    uint8_t tag = live_tag[idx];

    for (uint64_t k = 0; k < n; k++) {
        if (p[k] != tag) {
            CHECK(0, "stress: payload corrupted at free (size %llu)",
                  (unsigned long long)live_size[idx]);
            break;
        }
    }

    kfree(p);
    live_n--;
    live_ptr[idx] = live_ptr[live_n];
    live_size[idx] = live_size[live_n];
    live_tag[idx] = live_tag[live_n];
}

static void test_random_stress(void) {
    printf("\n=== Test 3: random allocation stress (%d ops) ===\n", STRESS_OPS);

    live_n = 0;
    int alloc_failures = 0;
    int overlaps = 0;

    for (int op = 0; op < STRESS_OPS; op++) {
        int do_alloc = (live_n == 0) || ((rand() % 100) < 60);

        if (do_alloc && live_n < MAX_LIVE) {
            uint64_t size;
            int r = rand() % 100;
            if (r < 55) {
                size = 1u + (uint64_t)(rand() % 4096);
            } else if (r < 98) {
                size = 4097u + (uint64_t)(rand() % 28672);
            } else {
                size = 40000u + (uint64_t)(rand() % 60000);
            }

            void* p = kmalloc((size_t)size);
            if (!p) {
                alloc_failures++;
                continue;
            }

            for (int j = 0; j < live_n; j++) {
                if (ranges_overlap((uintptr_t)p, size,
                                   (uintptr_t)live_ptr[j], live_size[j])) {
                    overlaps++;
                    break;
                }
            }

            uint8_t tag = (uint8_t)((op * 31 + 7) & 0xFF);
            uint64_t n = fill_len(size);
            for (uint64_t k = 0; k < n; k++) ((uint8_t*)p)[k] = tag;

            live_ptr[live_n] = p;
            live_size[live_n] = size;
            live_tag[live_n] = tag;
            live_n++;
        } else if (live_n > 0) {
            stress_free_one(rand() % live_n);
        }
    }

    while (live_n > 0) stress_free_one(live_n - 1);

    CHECK(alloc_failures == 0, "stress: %d allocation failures", alloc_failures);
    CHECK(overlaps == 0, "stress: %d overlapping live allocations", overlaps);

    uint64_t allocs = 0, frees = 0, bad = 0, live = 0;
    for (int i = 0; i < SLAB_N_SIZE_CLASSES; i++) {
        slab_stats_t st;
        if (slab_get_stats(i, &st) != 0) continue;
        allocs += st.allocs;
        frees += st.frees;
        bad += st.bad_frees;
        live += st.live_objs;
    }

    CHECK(live == 0, "stress: no live slab objects after cleanup");
    CHECK(allocs == frees, "stress: slab allocs==frees (%llu/%llu)",
          (unsigned long long)frees, (unsigned long long)allocs);
    CHECK(bad == 0, "stress: no rejected frees (%llu)", (unsigned long long)bad);

    uint64_t pmm_frames = pmm_stub_used_pages();
    uint64_t slab_frames = slab_bytes_owned() / 4096u;
    CHECK(pmm_frames == slab_frames,
          "stress: no PMM leak outside slabs (pmm %llu, slabs %llu)",
          (unsigned long long)pmm_frames, (unsigned long long)slab_frames);

    printf("  %llu slab allocations, %llu frames held by slab caches\n",
           (unsigned long long)allocs, (unsigned long long)pmm_frames);
}

/* ---------------------------------------------------------------- */
/*                     Test 4: error handling                       */
/* ---------------------------------------------------------------- */

static void test_error_handling(void) {
    printf("\n=== Test 4: error handling ===\n");

    struct kmem_cache* c = slab_caches[2]; /* 64-byte class */
    CHECK(c != NULL, "64-byte cache exists");
    if (!c) return;

    slab_stats_t before;
    slab_stats_t after;
    slab_get_stats(2, &before);

    void* p = kmem_cache_alloc(c);
    CHECK(p != NULL, "double-free probe allocation");
    if (!p) return;

    kmem_cache_free(c, p);
    kmem_cache_free(c, p); /* must be rejected, not corrupt the slab */

    slab_get_stats(2, &after);
    CHECK(after.bad_frees == before.bad_frees + 1,
          "double free rejected (%llu -> %llu)",
          (unsigned long long)before.bad_frees,
          (unsigned long long)after.bad_frees);
    CHECK(after.frees == before.frees + 1,
          "double free did not increment the free count");

    /* Null pointers must be ignored everywhere. */
    kfree(NULL);
    kmem_cache_free(c, NULL);
    CHECK(slab_free_ptr(NULL) == 0, "slab_free_ptr(NULL) returns 0");
    CHECK(1, "null-pointer frees are safe");

    /* A foreign stack pointer must not be treated as a slab object. */
    int local = 0;
    CHECK(slab_free_ptr(&local) == 0, "slab_free_ptr rejects stack memory");
}

/* ---------------------------------------------------------------- */
/*                    Test 5: PMM fallback + selftest               */
/* ---------------------------------------------------------------- */

static void test_large_and_selftest(void) {
    printf("\n=== Test 5: PMM fallback and mem_selftest ===\n");

    uint64_t frames_before = pmm_stub_used_pages();
    uint8_t* big = (uint8_t*)kmalloc(100000);
    CHECK(big != NULL, "kmalloc(100000) via PMM fallback");
    if (big) {
        for (uint64_t i = 0; i < 100000; i++) big[i] = (uint8_t)(i * 7u);
        int ok = 1;
        for (uint64_t i = 0; i < 100000; i++) {
            if (big[i] != (uint8_t)(i * 7u)) { ok = 0; break; }
        }
        CHECK(ok, "100000-byte PMM allocation survives writes");
        kfree(big);
    }
    CHECK(pmm_stub_used_pages() == frames_before,
          "PMM fallback returns its frames (before %llu, after %llu)",
          (unsigned long long)frames_before,
          (unsigned long long)pmm_stub_used_pages());

    void* zero = kmalloc(0);
    CHECK(zero != NULL, "kmalloc(0) returns a valid object");
    kfree(zero);

    CHECK(mem_selftest() == 0, "mem_selftest() passes");
}

/* ---------------------------------------------------------------- */
/*                              Main                                */
/* ---------------------------------------------------------------- */

int main(void) {
    printf("ARC OS slab allocator stress test (real kernel code)\n");

    srand(42);
    kmem_init_early();

    test_size_class_mapping();
    test_every_class();
    test_random_stress();
    test_error_handling();
    test_large_and_selftest();

    printf("\n=== Results ===\n");
    printf("  passed: %d\n", tests_passed);
    printf("  failed: %d\n", tests_failed);
    printf("  frames held: %llu (%llu KiB)\n",
           (unsigned long long)pmm_stub_used_pages(),
           (unsigned long long)(pmm_stub_used_pages() * 4));

    if (tests_failed == 0) {
        printf("\n  ALL TESTS PASSED\n");
        return 0;
    }
    printf("\n  %d TEST(S) FAILED\n", tests_failed);
    return 1;
}
