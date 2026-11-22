#include <stddef.h>
#include <stdint.h>

void terminal_putstring(const char* s) { (void)s; }
void terminal_print_int(uint64_t n) { (void)n; }
void terminal_print_hex(uint64_t n) { (void)n; }

void* pmm_alloc_pages(size_t count) {
    (void)count;
    return NULL;
}
void pmm_free_pages(void* addr, size_t count) { (void)addr; (void)count; }
void pmm_reserve_range(uint64_t base, uint64_t len) { (void)base; (void)len; }

void vmm_map_page(uint64_t vaddr, uint64_t paddr, uint8_t flags) {
    (void)vaddr; (void)paddr; (void)flags;
}
