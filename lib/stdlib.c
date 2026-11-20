/*
 * ARC OS - freestanding heap entry points.
 *
 * malloc/free are thin wrappers over the kernel slab heap so kernel code that
 * expects a C runtime can use the familiar names.  They are kernel-only; the
 * ring-3 shell is linked without libc.
 */
#include <stddef.h>

extern void* kmalloc(size_t size);
extern void kfree(void* ptr);

void* malloc(size_t size) {
    return kmalloc(size);
}

void free(void* ptr) {
    kfree(ptr);
}
