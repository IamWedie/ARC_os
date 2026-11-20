/*
 * Host-side stubs for the descriptor tests.
 *
 * kernel/fd.c owns the fd and OFD tables; these stubs stand in for the three
 * pieces it borrows from the kernel: the process table (so tests can allocate
 * a descriptor table), the console (so std-fd I/O can be observed), and the
 * filesystem slot API (so file-backed descriptors have storage).
 *
 * The fake slots mirror the contract the real backend provides:
 * fs_open_slot() resolves or creates a slot WITHOUT taking a reference. The
 * fd layer acquires exactly one reference per open file description and
 * releases it when the OFD's last descriptor closes; dup/clone only bump the
 * OFD refcount. A deleted slot keeps its data until its last reference is
 * dropped.
 */
#include <stddef.h>
#include <string.h>

#include "kernel.h"

#define FD_STUB_SLOT_DATA 4096

typedef struct {
    int used;
    int deleted;
    int refs;
    uint32_t size;
    char name[FS_MAX_FILENAME];
    char data[FD_STUB_SLOT_DATA];
} fd_stub_slot_t;

process_t processes[MAX_PROCESSES];
volatile int current_process;

static fd_stub_slot_t slots[FS_MAX_FILES];
static unsigned add_ref_calls;
static unsigned release_calls;

static char console_out[256];
static size_t console_out_len;

static const char console_in[] = "a\n";
static size_t console_in_pos;

void fd_stub_console_reset(void) {
    console_out_len = 0;
    console_out[0] = '\0';
    console_in_pos = 0;
}

const char* fd_stub_console_text(void) {
    console_out[console_out_len] = '\0';
    return console_out;
}

void terminal_write(const char* buf, size_t count) {
    for (size_t i = 0; i < count && console_out_len < sizeof(console_out) - 1; i++) {
        console_out[console_out_len++] = buf[i];
    }
    console_out[console_out_len] = '\0';
}

int kbd_getchar(void) {
    if (console_in[console_in_pos] == '\0') return -1;
    return (unsigned char)console_in[console_in_pos++];
}

/* Reset fake slots, slot counters, and console state between tests. */
void fd_stub_reset(void) {
    memset(slots, 0, sizeof(slots));
    add_ref_calls = 0;
    release_calls = 0;
    fd_stub_console_reset();
}

unsigned fd_stub_add_ref_calls(void) {
    return add_ref_calls;
}

unsigned fd_stub_release_calls(void) {
    return release_calls;
}

/* Total open references across every live slot; 0 means nothing leaked. */
int fd_stub_total_refs(void) {
    int total = 0;
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (slots[i].used) total += slots[i].refs;
    }
    return total;
}

/* Slot index backing `name`, or -1 if no live file has that name. */
int fd_stub_slot_for(const char* name) {
    if (!name) return -1;
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (!slots[i].used || slots[i].deleted) continue;
        if (strcmp(slots[i].name, name) == 0) return i;
    }
    return -1;
}

static int slot_valid(int slot) {
    return slot >= 0 && slot < FS_MAX_FILES && slots[slot].used;
}

static void slot_set_name(fd_stub_slot_t* slot, const char* name) {
    size_t i = 0;
    while (i < FS_MAX_FILENAME - 1 && name[i] != '\0') {
        slot->name[i] = name[i];
        i++;
    }
    slot->name[i] = '\0';
}

int fs_open_slot(const char* name, int create) {
    if (!name || name[0] == '\0') return -1;

    int existing = fd_stub_slot_for(name);
    if (existing >= 0) return existing;
    if (!create) return -1;

    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (slots[i].used) continue;
        memset(&slots[i], 0, sizeof(slots[i]));
        slots[i].used = 1;
        slot_set_name(&slots[i], name);
        return i;
    }
    return -1;
}

int fs_slot_read(int slot, uint32_t off, void* buf, size_t count) {
    if (!slot_valid(slot) || (!buf && count > 0)) return -1;
    if (count == 0 || off >= slots[slot].size) return 0;

    size_t avail = slots[slot].size - off;
    size_t n = count < avail ? count : avail;
    memcpy(buf, slots[slot].data + off, n);
    return (int)n;
}

int fs_slot_write(int slot, uint32_t off, const void* buf, size_t count) {
    if (!slot_valid(slot) || (!buf && count > 0)) return -1;
    if (count == 0) return 0;
    if (off > slots[slot].size) return -1;
    if (off >= FD_STUB_SLOT_DATA) return 0;

    size_t room = FD_STUB_SLOT_DATA - off;
    size_t n = count < room ? count : room;
    if (n > 0) memcpy(slots[slot].data + off, buf, n);
    if (off + n > slots[slot].size) slots[slot].size = (uint32_t)(off + n);
    return (int)n;
}

int fs_slot_size(int slot) {
    if (!slot_valid(slot)) return -1;
    return (int)slots[slot].size;
}

int fs_slot_truncate(int slot) {
    if (!slot_valid(slot)) return -1;
    slots[slot].size = 0;
    return 0;
}

int fs_slot_add_ref(int slot) {
    if (!slot_valid(slot)) return -1;
    slots[slot].refs++;
    add_ref_calls++;
    return 0;
}

void fs_slot_release(int slot) {
    if (!slot_valid(slot)) return;
    release_calls++;
    if (slots[slot].refs > 0) slots[slot].refs--;
    if (slots[slot].refs == 0 && slots[slot].deleted) {
        memset(&slots[slot], 0, sizeof(slots[slot]));
    }
}
