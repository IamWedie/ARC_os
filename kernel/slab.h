/*
 * ARC OS Slab Allocator
 *
 * Size-classed object caches backed by 4 KiB PMM pages.
 *
 * Layout of a small-object slab (one page):
 *
 *   +----------------------------+  <- page base (4 KiB aligned)
 *   | struct slab_meta           |     metadata + allocation bitmap
 *   +----------------------------+  <- SLAB_SMALL_OBJ_START
 *   | object 0                   |
 *   | object 1                   |
 *   | ...                        |
 *   +----------------------------+
 *
 * Free objects store the next-free pointer in their first 8 bytes.
 * Every object is tracked in a bitmap, so double frees are detected
 * instead of corrupting the freelist.
 *
 * Objects larger than SLAB_SMALL_MAX get a dedicated contiguous page
 * run with a header at the page-aligned base.
 *
 * Concurrency:
 *   - kmem_cache_alloc() takes no lock when the per-CPU active slab has
 *     a free object (the common case).
 *   - Slow paths and frees take the cache spinlock.
 *   - The kernel is uniprocessor today; the per-CPU slots make the
 *     structure ready for SMP. IRQ-disabling is the caller's job until
 *     the interrupt layer grows an irq-save spinlock.
 */
#ifndef SLAB_H
#define SLAB_H

#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------- */
/*                          Configuration                           */
/* ---------------------------------------------------------------- */

#define SLAB_N_SIZE_CLASSES 12
#define SLAB_MIN_OBJ_SIZE   16ULL
#define SLAB_MAX_OBJ_SIZE   32768ULL

/* Size class i: 16 << i  (16, 32, ..., 32768) */
#define SLAB_SIZE_CLASS(i) (SLAB_MIN_OBJ_SIZE << (i))

/* Per-CPU active-slab slots. Must be a power of two. */
#define SLAB_CPU_SLOTS 64

/* ---------------------------------------------------------------- */
/*                              Types                               */
/* ---------------------------------------------------------------- */

struct kmem_cache;

/* Minimal ticket-less spinlock. Only used for cold paths. */
typedef struct {
    volatile int locked;
} spinlock_t;

typedef struct {
    const char* name;
    uint64_t    obj_size;
    uint64_t    slabs;       /* page runs currently owned */
    uint64_t    allocs;      /* successful allocations */
    uint64_t    frees;       /* successful frees */
    uint64_t    bad_frees;   /* double/invalid frees rejected */
    uint64_t    live_objs;   /* allocs - frees */
    uint64_t    free_objs;   /* objects currently on freelists */
} slab_stats_t;

/* ---------------------------------------------------------------- */
/*                          Global caches                           */
/* ---------------------------------------------------------------- */

/* One cache per size class, created by kmem_init_early(). */
extern struct kmem_cache* slab_caches[SLAB_N_SIZE_CLASSES];

/* ---------------------------------------------------------------- */
/*                              API                                 */
/* ---------------------------------------------------------------- */

/* Create a cache for objects of `obj_size` bytes. Alignment must be a
 * power of two between 16 and 4096. Returns NULL on invalid input or
 * when the static cache-descriptor pool is exhausted. */
struct kmem_cache* kmem_cache_create(const char* name,
                                     uint64_t obj_size,
                                     uint64_t alignment);

/* Destroy a cache and return every page it owns to the PMM.
 * The caller must guarantee no live objects. */
void kmem_cache_destroy(struct kmem_cache* cache);

/* Allocate one object. Returns NULL only when the PMM is out of memory. */
void* kmem_cache_alloc(struct kmem_cache* cache);

/* Return one object to its cache. Invalid pointers are ignored. */
void kmem_cache_free(struct kmem_cache* cache, void* obj);

/* Create the 12 default size-class caches. Idempotent. */
void kmem_init_early(void);

/* SMP post-init hook (no-op until SMP exists). */
void kmem_init_smp(void);

/* Map a byte count to a size-class index, or -1 if it is too large. */
int slab_size_class(uint64_t size);

/* Try to free a pointer through the slab layer.
 * Returns 1 if the pointer belonged to a slab, 0 otherwise. */
int slab_free_ptr(void* ptr);

/* Copy statistics for size class `index` (0..11). Returns 0 on success. */
int slab_get_stats(int index, slab_stats_t* out);

/* Total bytes currently owned by all slab page runs (debug/tests). */
uint64_t slab_bytes_owned(void);

#endif /* SLAB_H */
