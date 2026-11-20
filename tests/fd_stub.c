/*
 * Host-side stubs for the descriptor tests.
 *
 * kernel/fd.c owns the fd and OFD tables; these stubs stand in for the two
 * pieces it borrows from the kernel: the process table (so tests can allocate
 * a descriptor table) and the console (so std-fd I/O can be observed).
 */
#include <stddef.h>

#include "kernel.h"

process_t processes[MAX_PROCESSES];
volatile int current_process;

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
