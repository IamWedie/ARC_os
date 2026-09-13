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

void* kmalloc(size_t size) {
    static uint8_t heap[0x100000];
    static size_t heap_offset = 0;
    if (heap_offset + size > sizeof(heap)) return 0;
    void* ptr = &heap[heap_offset];
    heap_offset += size;
    memset(ptr, 0, size);
    return ptr;
}

void kfree(void* ptr) {
    /* Simple allocator - no actual freeing */
}
