/*
 * ARC OS file descriptor test
 *
 * Links against the REAL kernel/fd.c with tests/fd_stub.c providing the
 * process table and console I/O. The std-fd tests pin the behaviour every
 * process inherits: fd 0/1/2 are console stdin/stdout/stderr, they can be
 * closed, and seek/size/truncate are meaningless for a console.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#undef SYS_OPEN /* MinGW stdio uses this name for _SYS_OPEN */
#include "kernel.h"

extern void fd_stub_console_reset(void);
extern const char* fd_stub_console_text(void);

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

static void test_std_fds(void) {
    printf("\n=== Test 1: standard descriptors ===\n");
    fd_stub_console_reset();
    fd_init_process(&processes[1]);

    CHECK(processes[1].fds[FD_STDIN].kind == FD_ENTRY_CONSOLE_IN,
          "fd 0 kind = %d", processes[1].fds[FD_STDIN].kind);
    CHECK(processes[1].fds[FD_STDOUT].kind == FD_ENTRY_CONSOLE_OUT,
          "fd 1 kind = %d", processes[1].fds[FD_STDOUT].kind);
    CHECK(processes[1].fds[FD_STDERR].kind == FD_ENTRY_CONSOLE_ERR,
          "fd 2 kind = %d", processes[1].fds[FD_STDERR].kind);
    CHECK(processes[1].fds[FD_STDIN].ofd == -1,
          "console fd 0 points at OFD %d", processes[1].fds[FD_STDIN].ofd);
    for (int fd = 3; fd < FD_MAX; fd++) {
        CHECK(processes[1].fds[fd].kind == FD_ENTRY_FREE,
              "fd %d should start free, kind = %d", fd,
              processes[1].fds[fd].kind);
    }

    CHECK(fd_write(1, FD_STDOUT, "hi", 2) == 2, "stdout write did not return 2");
    CHECK(strcmp(fd_stub_console_text(), "hi") == 0,
          "stdout captured \"%s\"", fd_stub_console_text());

    fd_stub_console_reset();
    CHECK(fd_write(1, FD_STDERR, "e", 1) == 1, "stderr write did not return 1");
    CHECK(strcmp(fd_stub_console_text(), "e") == 0,
          "stderr captured \"%s\"", fd_stub_console_text());

    char buf[4] = {0};
    CHECK(fd_read(1, FD_STDIN, buf, 1) == 1, "stdin read did not return 1");
    CHECK(buf[0] == 'a', "stdin read byte = 0x%02x", (unsigned char)buf[0]);

    CHECK(fd_seek(1, FD_STDOUT, 0, SEEK_SET) == -1,
          "seek on console stdout was accepted");
    CHECK(fd_size(1, FD_STDOUT) == -1, "size on console stdout was accepted");
    CHECK(fd_truncate(1, FD_STDOUT) == -1,
          "truncate on console stdout was accepted");

    CHECK(fd_close(1, FD_STDOUT) == 0, "close(stdout) failed");
    CHECK(processes[1].fds[FD_STDOUT].kind == FD_ENTRY_FREE,
          "closed fd 1 is still kind %d", processes[1].fds[FD_STDOUT].kind);
    CHECK(fd_write(1, FD_STDOUT, "x", 1) == -1,
          "write to closed stdout was accepted");
}

int main(void) {
    printf("ARC OS file descriptor tests\n");
    test_std_fds();

    printf("\n=== fd suite: %d checks, %d failures ===\n",
           tests_passed, tests_failed);
    return tests_failed ? 1 : 0;
}
