/*
 * ARC OS Slab Allocator Implementation
 *
 * See slab.h for the layout and concurrency contract.
 */
#include "slab.h"
#include "kernel.h"   /* pmm_alloc_pages / pmm_free_pages */

/* ---------------------------------------------------------------- */
/*                          Constants                               */
/* ---------------------------------------------------------------- */

#define SLAB_PAGE_SIZE    4096u
#define SLAB_OBJ_ALIGN    16u
#define SLAB_SMALL_MAX    2048u
#define SLAB_MAX_CACHES   32u
#define SLAB_BITMAP_WORDS 8u          /* 8 * 64 = 512 objects max/slab */

#define SLAB_MAGIC_SMALL  0x534C4142u /* "SLAB" */
#define SLAB_MAGIC_BIG    0x534C4247u /* "SLBG" */

/* ---------------------------------------------------------------- */
/*                        Internal layout                           */
/* ---------------------------------------------------------------- */

/* Common prefix so every page run can be linked into cache->all. */
struct slab_hdr {
    uint32_t          magic;
    uint32_t          aux;
    struct kmem_cache* cache;
    struct slab_hdr*  next_all;
};

/* One page holding many fixed-size objects. */
struct slab_meta {
    struct slab_hdr   h;
    void*             freelist;      /* first free object */
    struct slab_meta* next_partial;  /* cache->partial linkage */
    uint64_t          alloc_bits[SLAB_BITMAP_WORDS];
    uint32_t          obj_size;      /* stride, bytes */
    uint32_t          total;         /* objects in this slab */
    uint32_t          free_count;
    uint32_t          in_partial;
};

/* A contiguous page run holding one large object. */
struct slab_big {
    struct slab_hdr h;
    uint32_t        pages;
    uint32_t        pad;
    void*           obj;
};

#define SLAB_SMALL_OBJ_START \
    ((uint32_t)((sizeof(struct slab_meta) + (SLAB_OBJ_ALIGN - 1u)) & \
                ~(uint64_t)(SLAB_OBJ_ALIGN - 1u)))

#define SLAB_BIG_OBJ_START \
    ((uint32_t)((sizeof(struct slab_big) + (SLAB_OBJ_ALIGN - 1u)) & \
                ~(uint64_t)(SLAB_OBJ_ALIGN - 1u)))

/* The layout assumptions the allocator relies on. */
_Static_assert(SLAB_SMALL_OBJ_START + SLAB_MIN_OBJ_SIZE <= SLAB_PAGE_SIZE,
               "small-slab header must leave room for one object");
_Static_assert(((SLAB_PAGE_SIZE - SLAB_SMALL_OBJ_START) / SLAB_MIN_OBJ_SIZE)
                   <= (SLAB_BITMAP_WORDS * 64u),
               "allocation bitmap is too small for the smallest class");
_Static_assert(SLAB_BIG_OBJ_START + SLAB_MAX_OBJ_SIZE <= 9u * SLAB_PAGE_SIZE,
               "largest object must fit in the big-slab page run");

/* ---------------------------------------------------------------- */
/*                        Cache descriptor                          */
/* ---------------------------------------------------------------- */

struct kmem_cache {
    char              name[24];
    uint64_t          obj_size;   /* requested payload bytes */
    uint64_t          stride;     /* payload rounded to alignment */
    uint64_t          slabs;      /* page runs owned */
    uint64_t          allocs;
    uint64_t          frees;
    uint64_t          bad_frees;
    struct slab_hdr*  all;        /* every page run, for destroy */
    struct slab_meta* partial;    /* slabs with free objects */
    struct slab_meta* cpu_slab[SLAB_CPU_SLOTS];
    spinlock_t        lock;
    uint32_t          in_use;
    uint32_t          pad;
};

/* ---------------------------------------------------------------- */
/*                          Static state                            */
/* ---------------------------------------------------------------- */

static struct kmem_cache cache_pool[SLAB_MAX_CACHES];

struct kmem_cache* slab_caches[SLAB_N_SIZE_CLASSES];

/* ---------------------------------------------------------------- */
/*                          Small helpers                           */
/* ---------------------------------------------------------------- */

static uint64_t slab_align_up(uint64_t x, uint64_t align) {
    return (x + align - 1u) & ~(align - 1u);
}

/* volatile keeps GCC from turning this into a memset() libcall, which
 * the freestanding kernel does not link against. */
static void slab_memzero(void* p, uint64_t n) {
    volatile uint8_t* b = (volatile uint8_t*)p;
    while (n--) *b++ = 0;
}

static void slab_copy_name(char* dst, const char* src, uint32_t cap) {
    uint32_t i = 0;
    if (src) {
        while (src[i] && (i + 1u) < cap) {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = '\0';
}

static void slab_lock(spinlock_t* lock) {
    while (__atomic_exchange_n(&lock->locked, 1, __ATOMIC_ACQUIRE)) {
        __asm__ volatile("pause");
    }
}

static void slab_unlock(spinlock_t* lock) {
    __atomic_store_n(&lock->locked, 0, __ATOMIC_RELEASE);
}

/* The kernel is uniprocessor today; SMP will return the LAPIC id here. */
static uint32_t slab_cpu_id(void) {
    return 0;
}

static int slab_bit_test(const struct slab_meta* m, uint32_t idx) {
    return (int)((m->alloc_bits[idx >> 6] >> (idx & 63u)) & 1u);
}

static void slab_bit_set(struct slab_meta* m, uint32_t idx) {
    m->alloc_bits[idx >> 6] |= (1ULL << (idx & 63u));
}

static void slab_bit_clear(struct slab_meta* m, uint32_t idx) {
    m->alloc_bits[idx >> 6] &= ~(1ULL << (idx & 63u));
}

/* ---------------------------------------------------------------- */
/*                        Page-run constructors                     */
/* ---------------------------------------------------------------- */

static struct slab_meta* slab_new_small(struct kmem_cache* c) {
    void* base = pmm_alloc_pages(1);
    if (!base) return NULL;

    slab_memzero(base, SLAB_PAGE_SIZE);

    struct slab_meta* m = (struct slab_meta*)base;
    m->h.magic = SLAB_MAGIC_SMALL;
    m->h.cache = c;
    m->obj_size = (uint32_t)c->stride;

    uint32_t start = SLAB_SMALL_OBJ_START;
    m->total = (SLAB_PAGE_SIZE - start) / m->obj_size;
    if (m->total == 0 || m->total > (SLAB_BITMAP_WORDS * 64u)) {
        pmm_free_pages(base, 1);
        return NULL;
    }

    m->free_count = m->total;
    uint8_t* first = (uint8_t*)base + start;
    for (uint32_t i = m->total; i > 0; i--) {
        void* obj = first + (uint64_t)(i - 1u) * m->obj_size;
        *(void**)obj = m->freelist;
        m->freelist = obj;
    }

    m->h.next_all = c->all;
    c->all = &m->h;
    c->slabs++;
    return m;
}

static struct slab_big* slab_new_big(struct kmem_cache* c) {
    uint64_t bytes = (uint64_t)SLAB_BIG_OBJ_START + c->stride;
    uint32_t pages = (uint32_t)((bytes + SLAB_PAGE_SIZE - 1u) / SLAB_PAGE_SIZE);

    void* base = pmm_alloc_pages(pages);
    if (!base) return NULL;

    slab_memzero(base, (uint64_t)pages * SLAB_PAGE_SIZE);

    struct slab_big* b = (struct slab_big*)base;
    b->h.magic = SLAB_MAGIC_BIG;
    b->h.cache = c;
    b->pages = pages;
    b->obj = (uint8_t*)base + SLAB_BIG_OBJ_START;

    b->h.next_all = c->all;
    c->all = &b->h;
    c->slabs++;
    return b;
}

/* ---------------------------------------------------------------- */
/*                          Allocation                              */
/* ---------------------------------------------------------------- */

static void* slab_small_alloc_slow(struct kmem_cache* c, uint32_t cpu) {
    slab_lock(&c->lock);

    struct slab_meta* m = c->partial;
    if (m) {
        c->partial = m->next_partial;
        m->next_partial = NULL;
        m->in_partial = 0;
    } else {
        m = slab_new_small(c);
        if (!m) {
            slab_unlock(&c->lock);
            return NULL;
        }
    }

    /* The previous active slab, if any, is full. Keep it as a full slab;
     * only the 'all' list references it now. */
    c->cpu_slab[cpu] = m;

    void* obj = m->freelist;
    if (!obj) { /* should not happen: partial slabs have free objects */
        slab_unlock(&c->lock);
        return NULL;
    }
    m->freelist = *(void**)obj;
    m->free_count--;

    uint32_t idx = (uint32_t)(((uintptr_t)obj -
                    ((uintptr_t)m + SLAB_SMALL_OBJ_START)) / c->stride);
    slab_bit_set(m, idx);

    c->allocs++;
    slab_unlock(&c->lock);
    return obj;
}

void* kmem_cache_alloc(struct kmem_cache* c) {
    if (!c || !c->in_use) return NULL;

    if (c->stride > SLAB_SMALL_MAX) {
        slab_lock(&c->lock);
        struct slab_big* b = slab_new_big(c);
        if (b) c->allocs++;
        slab_unlock(&c->lock);
        return b ? b->obj : NULL;
    }

    uint32_t cpu = slab_cpu_id() % SLAB_CPU_SLOTS;
    struct slab_meta* m = c->cpu_slab[cpu];

    /* Fast path: per-CPU active slab has a free object. */
    if (m && m->free_count) {
        void* obj = m->freelist;
        m->freelist = *(void**)obj;
        m->free_count--;

        uint32_t idx = (uint32_t)(((uintptr_t)obj -
                        ((uintptr_t)m + SLAB_SMALL_OBJ_START)) / c->stride);
        slab_bit_set(m, idx);

        c->allocs++;
        return obj;
    }

    return slab_small_alloc_slow(c, cpu);
}

/* ---------------------------------------------------------------- */
/*                            Freeing                               */
/* ---------------------------------------------------------------- */

static void slab_big_free(struct kmem_cache* c, void* obj) {
    uintptr_t base = (uintptr_t)obj & ~(uintptr_t)(SLAB_PAGE_SIZE - 1u);
    struct slab_big* b = (struct slab_big*)base;

    if (b->h.magic != SLAB_MAGIC_BIG || b->h.cache != c || b->obj != obj) {
        c->bad_frees++;
        return;
    }

    slab_lock(&c->lock);

    struct slab_hdr** pp = &c->all;
    while (*pp && *pp != &b->h) pp = &(*pp)->next_all;
    if (!*pp) {
        c->bad_frees++;
        slab_unlock(&c->lock);
        return;
    }
    *pp = b->h.next_all;
    if (c->slabs) c->slabs--;
    c->frees++;

    uint32_t pages = b->pages;
    slab_unlock(&c->lock);

    pmm_free_pages(b, pages);
}

void kmem_cache_free(struct kmem_cache* c, void* obj) {
    if (!c || !obj || !c->in_use) return;

    if (c->stride > SLAB_SMALL_MAX) {
        slab_big_free(c, obj);
        return;
    }

    uintptr_t base = (uintptr_t)obj & ~(uintptr_t)(SLAB_PAGE_SIZE - 1u);
    struct slab_meta* m = (struct slab_meta*)base;

    if (m->h.magic != SLAB_MAGIC_SMALL || m->h.cache != c) {
        c->bad_frees++;
        return;
    }

    uint64_t off = (uintptr_t)obj - (base + SLAB_SMALL_OBJ_START);
    if ((off % c->stride) != 0) {
        c->bad_frees++;
        return;
    }
    uint32_t idx = (uint32_t)(off / c->stride);
    if (idx >= m->total) {
        c->bad_frees++;
        return;
    }

    slab_lock(&c->lock);

    if (!slab_bit_test(m, idx)) { /* double free or forged pointer */
        c->bad_frees++;
        slab_unlock(&c->lock);
        return;
    }
    slab_bit_clear(m, idx);

    int was_full = (m->free_count == 0);
    *(void**)obj = m->freelist;
    m->freelist = obj;
    m->free_count++;

    /* If an object returns to a full slab, make it allocatable again.
     * (UP: the active slab is this CPU's; SMP needs ownership tracking.) */
    if (was_full && m != c->cpu_slab[slab_cpu_id() % SLAB_CPU_SLOTS] &&
        !m->in_partial) {
        m->next_partial = c->partial;
        c->partial = m;
        m->in_partial = 1;
    }

    c->frees++;
    slab_unlock(&c->lock);
}

int slab_free_ptr(void* ptr) {
    if (!ptr) return 0;

    uintptr_t base = (uintptr_t)ptr & ~(uintptr_t)(SLAB_PAGE_SIZE - 1u);
    uint32_t magic = *(volatile uint32_t*)base;

    if (magic == SLAB_MAGIC_SMALL) {
        struct slab_meta* m = (struct slab_meta*)base;
        if (!m->h.cache) return 0;
        kmem_cache_free(m->h.cache, ptr);
        return 1;
    }
    if (magic == SLAB_MAGIC_BIG) {
        struct slab_big* b = (struct slab_big*)base;
        if (!b->h.cache) return 0;
        kmem_cache_free(b->h.cache, ptr);
        return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- */
/*                       Create / destroy                           */
/* ---------------------------------------------------------------- */

struct kmem_cache* kmem_cache_create(const char* name,
                                     uint64_t obj_size,
                                     uint64_t alignment) {
    if (obj_size == 0 || obj_size > SLAB_MAX_OBJ_SIZE) return NULL;

    if (alignment == 0) alignment = SLAB_OBJ_ALIGN;
    if (alignment < SLAB_OBJ_ALIGN) alignment = SLAB_OBJ_ALIGN;
    if (alignment > SLAB_PAGE_SIZE) return NULL;
    if (alignment & (alignment - 1u)) return NULL; /* not power of two */

    uint64_t stride = slab_align_up(obj_size, alignment);
    if (stride > SLAB_MAX_OBJ_SIZE) return NULL;

    struct kmem_cache* c = NULL;
    for (uint32_t i = 0; i < SLAB_MAX_CACHES; i++) {
        if (!cache_pool[i].in_use) {
            c = &cache_pool[i];
            break;
        }
    }
    if (!c) return NULL;

    slab_memzero(c, sizeof(*c));
    c->in_use = 1;
    c->obj_size = obj_size;
    c->stride = stride;
    slab_copy_name(c->name, name, sizeof(c->name));

    return c;
}

void kmem_cache_destroy(struct kmem_cache* c) {
    if (!c || !c->in_use) return;

    for (uint32_t i = 0; i < SLAB_N_SIZE_CLASSES; i++) {
        if (slab_caches[i] == c) slab_caches[i] = NULL;
    }

    struct slab_hdr* h = c->all;
    while (h) {
        struct slab_hdr* next = h->next_all;
        if (h->magic == SLAB_MAGIC_SMALL) {
            pmm_free_pages(h, 1);
        } else if (h->magic == SLAB_MAGIC_BIG) {
            struct slab_big* b = (struct slab_big*)h;
            pmm_free_pages(h, b->pages);
        }
        h = next;
    }

    slab_memzero(c, sizeof(*c));
}

/* ---------------------------------------------------------------- */
/*                         Initialization                           */
/* ---------------------------------------------------------------- */

static const char* const slab_class_names[SLAB_N_SIZE_CLASSES] = {
    "slab-16",   "slab-32",   "slab-64",    "slab-128",
    "slab-256",  "slab-512",  "slab-1024",  "slab-2048",
    "slab-4096", "slab-8192", "slab-16384", "slab-32768"
};

void kmem_init_early(void) {
    for (int i = 0; i < SLAB_N_SIZE_CLASSES; i++) {
        if (!slab_caches[i]) {
            slab_caches[i] = kmem_cache_create(slab_class_names[i],
                                               SLAB_SIZE_CLASS(i),
                                               SLAB_OBJ_ALIGN);
        }
    }
}

void kmem_init_smp(void) {
    /* Per-CPU slots already exist; nothing to do until SMP bring-up. */
}

/* ---------------------------------------------------------------- */
/*                          Introspection                           */
/* ---------------------------------------------------------------- */

int slab_size_class(uint64_t size) {
    for (int i = 0; i < SLAB_N_SIZE_CLASSES; i++) {
        if (size <= SLAB_SIZE_CLASS(i)) return i;
    }
    return -1;
}

int slab_get_stats(int index, slab_stats_t* out) {
    if (!out || index < 0 || index >= SLAB_N_SIZE_CLASSES) return -1;
    struct kmem_cache* c = slab_caches[index];
    if (!c) return -1;

    out->name = c->name;
    out->obj_size = c->obj_size;
    out->slabs = c->slabs;
    out->allocs = c->allocs;
    out->frees = c->frees;
    out->bad_frees = c->bad_frees;
    out->live_objs = c->allocs - c->frees;
    out->free_objs = 0;

    for (struct slab_hdr* h = c->all; h; h = h->next_all) {
        if (h->magic == SLAB_MAGIC_SMALL) {
            struct slab_meta* m = (struct slab_meta*)h;
            out->free_objs += m->free_count;
        }
    }
    return 0;
}

uint64_t slab_bytes_owned(void) {
    uint64_t total = 0;
    for (int i = 0; i < SLAB_N_SIZE_CLASSES; i++) {
        struct kmem_cache* c = slab_caches[i];
        if (!c) continue;
        for (struct slab_hdr* h = c->all; h; h = h->next_all) {
            if (h->magic == SLAB_MAGIC_SMALL) {
                total += SLAB_PAGE_SIZE;
            } else if (h->magic == SLAB_MAGIC_BIG) {
                total += (uint64_t)((struct slab_big*)h)->pages * SLAB_PAGE_SIZE;
            }
        }
    }
    return total;
}
