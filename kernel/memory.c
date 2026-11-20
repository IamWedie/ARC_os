/*
 * ARC OS Kernel Heap
 *
 * kmalloc/kfree sit on top of the slab allocator for every request up to
 * SLAB_MAX_OBJ_SIZE (32 KiB). Larger requests are served straight from
 * the PMM with a small header so kfree can recover the page count.
 */
#include "kernel.h"
#include "slab.h"

#define PMM_ALLOC_MAGIC 0x504D4D42u  /* "PMMB" */
#define PMM_ALLOC_HDR   16u
#define PAGE_SIZE       4096u

struct pmm_alloc_hdr {
    uint32_t magic;
    uint32_t pages;
    uint64_t bytes;
};

/* ---------------------------------------------------------------- */
/*                          Helpers                                 */
/* ---------------------------------------------------------------- */

/* volatile prevents GCC from emitting a memset() libcall. */
static void mem_zero(void* p, uint64_t n) {
    volatile uint8_t* b = (volatile uint8_t*)p;
    while (n--) *b++ = 0;
}

static int mem_check_pattern(const uint8_t* p, uint64_t n, uint8_t want) {
    for (uint64_t i = 0; i < n; i++) {
        if (p[i] != want) return -1;
    }
    return 0;
}

/* ---------------------------------------------------------------- */
/*                          kmalloc / kfree                         */
/* ---------------------------------------------------------------- */

void* kmalloc(size_t size) {
    if (size == 0) size = 1;

    /* Lazy init keeps early callers safe even if boot forgot the call. */
    if (!slab_caches[0]) kmem_init_early();

    if ((uint64_t)size <= SLAB_MAX_OBJ_SIZE) {
        int cls = slab_size_class((uint64_t)size);
        if (cls >= 0 && slab_caches[cls]) {
            void* obj = kmem_cache_alloc(slab_caches[cls]);
            if (obj) return obj;
            /* Out of slab memory: fall through to the page allocator. */
        }
    }

    uint64_t bytes = (uint64_t)size;
    uint64_t pages64 = (bytes + PMM_ALLOC_HDR + PAGE_SIZE - 1u) / PAGE_SIZE;
    if (pages64 == 0 || pages64 > 0x7FFFFFFFu) return NULL;

    void* base = pmm_alloc_pages((size_t)pages64);
    if (!base) return NULL;

    struct pmm_alloc_hdr* h = (struct pmm_alloc_hdr*)base;
    h->magic = PMM_ALLOC_MAGIC;
    h->pages = (uint32_t)pages64;
    h->bytes = bytes;

    return (uint8_t*)base + PMM_ALLOC_HDR;
}

void kfree(void* ptr) {
    if (!ptr) return;

    /* Slab objects carry their own identification at the page base. */
    if (slab_free_ptr(ptr)) return;

    uintptr_t base = (uintptr_t)ptr & ~(uintptr_t)(PAGE_SIZE - 1u);
    struct pmm_alloc_hdr* h = (struct pmm_alloc_hdr*)base;

    if (h->magic != PMM_ALLOC_MAGIC ||
        (void*)(base + PMM_ALLOC_HDR) != ptr) {
        return; /* foreign or already-freed pointer: ignore */
    }

    uint32_t pages = h->pages;
    h->magic = 0;                /* poison before returning to PMM */
    pmm_free_pages((void*)base, pages);
}

/* ---------------------------------------------------------------- */
/*                          Self-test                               */
/* ---------------------------------------------------------------- */

int mem_selftest(void) {
    static const size_t sizes[] = {
        1, 16, 17, 64, 255, 1024, 2048, 4096, 20000, 65536
    };
    enum { N = (int)(sizeof(sizes) / sizeof(sizes[0])) };
    void* ptrs[N];

    for (int i = 0; i < N; i++) ptrs[i] = NULL;

    for (int i = 0; i < N; i++) {
        ptrs[i] = kmalloc(sizes[i]);
        if (!ptrs[i]) goto fail;
        mem_zero(ptrs[i], sizes[i]);
        for (size_t j = 0; j < sizes[i]; j++) {
            ((uint8_t*)ptrs[i])[j] = (uint8_t)(0xA5u ^ (uint8_t)j);
        }
    }

    for (int i = 0; i < N; i++) {
        const uint8_t* p = (const uint8_t*)ptrs[i];
        for (size_t j = 0; j < sizes[i]; j++) {
            if (p[j] != (uint8_t)(0xA5u ^ (uint8_t)j)) goto fail;
        }
    }

    for (int i = 0; i < N; i++) {
        kfree(ptrs[i]);
        ptrs[i] = NULL;
    }

    /* Reuse: two live small allocations must be distinct. */
    void* a = kmalloc(64);
    void* b = kmalloc(64);
    if (!a || !b || a == b) {
        if (a) kfree(a);
        if (b) kfree(b);
        return -1;
    }
    mem_zero(a, 64);
    if (mem_check_pattern((const uint8_t*)a, 64, 0) != 0) {
        kfree(a);
        kfree(b);
        return -1;
    }
    kfree(a);
    kfree(b);

    return 0;

fail:
    for (int i = 0; i < N; i++) {
        if (ptrs[i]) kfree(ptrs[i]);
    }
    return -1;
}
