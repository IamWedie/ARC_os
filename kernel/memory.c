#include "kernel.h"

void* memset(void* s, int c, size_t n) {
    unsigned char* p = (unsigned char*)s;
    for (size_t i = 0; i < n; i++) p[i] = (unsigned char)c;
    return s;
}

void* memcpy(void* dest, const void* src, size_t n) {
    unsigned char* d = (unsigned char*)dest;
    const unsigned char* sr = (const unsigned char*)src;
    for (size_t i = 0; i < n; i++) d[i] = sr[i];
    return dest;
}

void* memmove(void* dest, const void* src, size_t n) {
    unsigned char* d = (unsigned char*)dest;
    const unsigned char* sr = (const unsigned char*)src;
    if (d < sr) {
        for (size_t i = 0; i < n; i++) d[i] = sr[i];
    } else {
        for (size_t i = n; i > 0; i--) d[i-1] = sr[i-1];
    }
    return dest;
}

int strlen(const char* s) {
    int len = 0;
    while (s[len]) len++;
    return len;
}

int strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

/* Frame-backed first-fit heap. Each block has a header; blocks are kept
 * in an address-sorted doubly linked list and coalesced on free. */
typedef struct block_hdr {
    size_t size;              /* usable bytes (excludes this header) */
    struct block_hdr* next;
    struct block_hdr* prev;
    int free;
} block_hdr_t;

#define BLOCK_ALIGN 16

static block_hdr_t* heap_root;

static inline size_t block_align(size_t n) {
    return (n + (BLOCK_ALIGN - 1)) & ~(size_t)(BLOCK_ALIGN - 1);
}

static block_hdr_t* heap_extend(size_t min_usable) {
    size_t pages = 1 + (min_usable + sizeof(block_hdr_t) + 4095) / 4096;
    void* mem = pmm_alloc_pages(pages);
    if (!mem) return NULL;
    size_t total = pages * 4096;
    block_hdr_t* b = (block_hdr_t*)mem;
    b->size = total - sizeof(block_hdr_t);
    b->free = 1;
    b->next = NULL;
    b->prev = NULL;

    if (!heap_root) {
        heap_root = b;
        return b;
    }
    if ((uintptr_t)b < (uintptr_t)heap_root) {
        b->next = heap_root;
        heap_root->prev = b;
        heap_root = b;
        return b;
    }
    block_hdr_t* cur = heap_root;
    while (cur->next && (uintptr_t)cur->next < (uintptr_t)b)
        cur = cur->next;
    b->next = cur->next;
    if (b->next) b->next->prev = b;
    b->prev = cur;
    cur->next = b;
    return b;
}

void* kmalloc(size_t size) {
    if (size == 0) size = 1;
    size = block_align(size);

    preempt_disable();
    block_hdr_t* b = heap_root;
    while (b) {
        if (b->free && b->size >= size) {
            size_t spare = b->size - size;
            if (spare >= sizeof(block_hdr_t) + BLOCK_ALIGN) {
                block_hdr_t* n = (block_hdr_t*)((uint8_t*)(b + 1) + size);
                n->size = spare - sizeof(block_hdr_t);
                n->free = 1;
                n->next = b->next;
                if (n->next) n->next->prev = n;
                n->prev = b;
                b->next = n;
                b->size = size;
            }
            b->free = 0;
            memset(b + 1, 0, b->size);
            preempt_enable();
            return (void*)(b + 1);
        }
        b = b->next;
    }

    b = heap_extend(size);
    if (!b) return NULL;
    /* heap_extend returns a free block; split and hand out front part. */
    size_t spare = b->size - size;
    if (spare >= sizeof(block_hdr_t) + BLOCK_ALIGN) {
        block_hdr_t* n = (block_hdr_t*)((uint8_t*)(b + 1) + size);
        n->size = spare - sizeof(block_hdr_t);
        n->free = 1;
        n->next = b->next;
        if (n->next) n->next->prev = n;
        n->prev = b;
        b->next = n;
        b->size = size;
    } else {
        b->size -= spare; /* keep b->usable == requested block exactly */
    }
    b->free = 0;
    memset(b + 1, 0, b->size);
    preempt_enable();
    return (void*)(b + 1);
}

void kfree(void* ptr) {
    if (!ptr) return;
    preempt_disable();
    block_hdr_t* b = (block_hdr_t*)ptr - 1;
    if (b->free) {
        preempt_enable();
        return;
    }
    b->free = 1;

    /* coalesce with previous */
    if (b->prev && b->prev->free) {
        b->prev->size += b->size + sizeof(block_hdr_t);
        b->prev->next = b->next;
        if (b->next) b->next->prev = b->prev;
        b = b->prev;
    }
    /* coalesce with next */
    if (b->next && b->next->free) {
        b->size += b->next->size + sizeof(block_hdr_t);
        b->next = b->next->next;
        if (b->next) b->next->prev = b;
    }
    preempt_enable();
}

int mem_selftest(void) {
    void* a = kmalloc(64);
    void* b = kmalloc(4096);
    void* c = kmalloc(16);
    if (!a || !b || !c) return -1;
    for (int i = 0; i < 64; i++)
        if (((char*)a)[i] != 0) return -2;
    kfree(b);
    void* d = kmalloc(2048);          /* must reuse the freed 4096 block */
    if (!d) return -3;
    uintptr_t ub = (uintptr_t)b;
    if ((uintptr_t)d < ub || (uintptr_t)d >= ub + 4096) return -4;
    kfree(a); kfree(c); kfree(d);
    return 0;
}
