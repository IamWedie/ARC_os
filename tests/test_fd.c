/*
 * ARC OS file descriptor test
 *
 * Links against the REAL kernel/fd.c with tests/fd_stub.c providing the
 * process table, console I/O, and fake filesystem slots. The std-fd tests pin
 * the behaviour every process inherits: fd 0/1/2 are console stdin/stdout/
 * stderr, they can be closed, and seek/size/truncate are meaningless for a
 * console. The file-backed tests pin the open file description (OFD) rules:
 * independent opens have independent offsets, dup/clone share an OFD, access
 * flags are enforced, and the fixed pools fail cleanly without leaking slot
 * references.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#undef SYS_OPEN /* MinGW stdio uses this name for _SYS_OPEN */
#include "kernel.h"

extern void fd_stub_console_reset(void);
extern const char* fd_stub_console_text(void);
extern void fd_stub_reset(void);
extern unsigned fd_stub_add_ref_calls(void);
extern unsigned fd_stub_release_calls(void);
extern int fd_stub_total_refs(void);
extern int fd_stub_slot_for(const char* name);

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

static void test_independent_offsets(void) {
    printf("\n=== Test 2: independent offsets ===\n");
    fd_stub_reset();
    fd_init_process(&processes[1]);

    int a = fd_open(1, "shared", O_RDWR | O_CREAT);
    int b = fd_open(1, "shared", O_RDWR | O_CREAT);
    CHECK(a >= 0, "first open of \"shared\" failed");
    CHECK(b >= 0, "second open of \"shared\" failed");
    CHECK(a != b, "two opens returned the same fd %d", a);
    CHECK(fd_stub_slot_for("shared") >= 0, "\"shared\" has no backing slot");

    if (a >= 0 && b >= 0) {
        CHECK(processes[1].fds[a].ofd != processes[1].fds[b].ofd,
              "separate opens shared OFD %d", processes[1].fds[a].ofd);
        CHECK(processes[1].fds[a].kind == FD_ENTRY_FILE,
              "fd %d kind = %d", a, processes[1].fds[a].kind);
        CHECK(fd_write(1, a, "hello", 5) == 5, "write via first fd failed");
        CHECK(fd_size(1, b) == 5, "size via second fd = %d", fd_size(1, b));

        char buf[8] = {0};
        CHECK(fd_read(1, b, buf, 5) == 5, "second fd read did not return 5");
        CHECK(memcmp(buf, "hello", 5) == 0,
              "second fd read \"%.5s\" instead of \"hello\"", buf);
        CHECK(fd_read(1, b, buf, 1) == 0,
              "second fd was not at EOF after its own read");

        CHECK(fd_seek(1, a, 0, SEEK_SET) == 0,
              "seek first fd back to start failed");
        memset(buf, 0, sizeof(buf));
        CHECK(fd_read(1, a, buf, 5) == 5, "first fd re-read did not return 5");
        CHECK(memcmp(buf, "hello", 5) == 0,
              "first fd re-read \"%.5s\" instead of \"hello\"", buf);
    }

    if (a >= 0) CHECK(fd_close(1, a) == 0, "close first fd failed");
    if (b >= 0) CHECK(fd_close(1, b) == 0, "close second fd failed");
    CHECK(fd_stub_total_refs() == 0, "slot refs after close = %d",
          fd_stub_total_refs());
    fd_close_all(&processes[1]);
}

static void test_dup_shares_offset(void) {
    printf("\n=== Test 3: dup shares offset ===\n");
    fd_stub_reset();
    fd_init_process(&processes[1]);

    int a = fd_open(1, "dup", O_RDWR | O_CREAT);
    CHECK(a >= 0, "open \"dup\" failed");
    if (a >= 0) {
        CHECK(fd_write(1, a, "0123456789", 10) == 10, "initial write failed");
        CHECK(fd_seek(1, a, 2, SEEK_SET) == 2, "seek to 2 failed");

        int d = fd_dup(1, a);
        CHECK(d >= 0, "dup failed");
        CHECK(d != a, "dup returned the original fd %d", d);
        if (d >= 0) {
            CHECK(processes[1].fds[d].ofd == processes[1].fds[a].ofd,
                  "dup points at OFD %d, original at %d",
                  processes[1].fds[d].ofd, processes[1].fds[a].ofd);
            CHECK(fd_write(1, d, "XY", 2) == 2, "write via duplicate failed");
            CHECK(fd_size(1, d) == 10, "size after overwrite = %d", fd_size(1, d));

            char c = 0;
            CHECK(fd_read(1, a, &c, 1) == 1 && c == '4',
                  "original read at shared offset got '%c'", c);
            CHECK(fd_read(1, d, &c, 1) == 1 && c == '5',
                  "duplicate read at shared offset got '%c'", c);

            CHECK(fd_close(1, a) == 0, "close original failed");
            CHECK(fd_stub_total_refs() == 1, "slot refs after close = %d",
                  fd_stub_total_refs());
            CHECK(fd_read(1, d, &c, 1) == 1 && c == '6',
                  "duplicate after original close got '%c'", c);
            CHECK(fd_close(1, d) == 0, "close duplicate failed");
            CHECK(fd_stub_total_refs() == 0, "slot refs after both closes = %d",
                  fd_stub_total_refs());
        }
    }
    fd_close_all(&processes[1]);
}

static void test_close_std_fd_reuses_number(void) {
    printf("\n=== Test 4: closing a std fd reuses its number ===\n");
    fd_stub_reset();
    fd_init_process(&processes[1]);

    CHECK(fd_close(1, FD_STDOUT) == 0, "close(stdout) failed");
    int f = fd_open(1, "reuse.txt", O_RDWR | O_CREAT);
    CHECK(f == FD_STDOUT, "open after closing stdout returned fd %d", f);

    fd_stub_console_reset();
    CHECK(fd_write(1, f, "F", 1) == 1, "file write did not return 1");
    CHECK(fd_stub_console_text()[0] == '\0',
          "write went to console \"%s\"", fd_stub_console_text());
    CHECK(fd_size(1, f) == 1, "file size = %d", fd_size(1, f));
    CHECK(fd_seek(1, f, 0, SEEK_SET) == 0, "seek file to 0 failed");

    char c = 0;
    CHECK(fd_read(1, f, &c, 1) == 1 && c == 'F',
          "file read back '%c'", c);

    CHECK(fd_close(1, f) == 0, "close reused fd failed");
    CHECK(fd_stub_total_refs() == 0, "slot refs after close = %d",
          fd_stub_total_refs());
    fd_close_all(&processes[1]);
}

static void test_access_flags(void) {
    printf("\n=== Test 5: access flags ===\n");
    fd_stub_reset();
    fd_init_process(&processes[1]);

    int w = fd_open(1, "acc", O_WRONLY | O_CREAT);
    CHECK(w >= 0, "O_WRONLY|O_CREAT open failed");
    if (w >= 0) {
        char c = 0;
        CHECK(fd_read(1, w, &c, 1) == -1, "O_WRONLY read was accepted");
        CHECK(fd_write(1, w, "abc", 3) == 3, "O_WRONLY write failed");
    }

    int r = fd_open(1, "acc", O_RDONLY);
    CHECK(r >= 0, "O_RDONLY open failed");
    if (r >= 0) {
        CHECK(fd_write(1, r, "x", 1) == -1, "O_RDONLY write was accepted");
        char c = 0;
        CHECK(fd_read(1, r, &c, 1) == 1 && c == 'a',
              "O_RDONLY read got '%c'", c);
    }

    int c = fd_open(1, "created", O_RDONLY | O_CREAT);
    CHECK(c >= 0, "O_CREAT open failed");
    if (c >= 0) {
        CHECK(fd_size(1, c) == 0, "new file size = %d", fd_size(1, c));
    }

    int t = fd_open(1, "acc", O_WRONLY | O_TRUNC);
    CHECK(t >= 0, "O_TRUNC open failed");
    if (t >= 0) {
        CHECK(fd_size(1, t) == 0, "size after O_TRUNC = %d", fd_size(1, t));
        CHECK(fd_write(1, t, "z", 1) == 1, "write after O_TRUNC failed");
        CHECK(fd_size(1, t) == 1, "size after post-truncate write = %d",
              fd_size(1, t));
    }

    fd_close_all(&processes[1]);
    CHECK(fd_stub_total_refs() == 0, "slot refs after close-all = %d",
          fd_stub_total_refs());
}

static void test_seek_bounds(void) {
    printf("\n=== Test 6: seek bounds ===\n");
    fd_stub_reset();
    fd_init_process(&processes[1]);

    int f = fd_open(1, "seek", O_RDWR | O_CREAT);
    CHECK(f >= 0, "open \"seek\" failed");
    if (f >= 0) {
        CHECK(fd_write(1, f, "0123456789", 10) == 10, "write failed");
        CHECK(fd_seek(1, f, 0, SEEK_SET) == 0, "SEEK_SET 0 failed");

        char c = 0;
        CHECK(fd_read(1, f, &c, 1) == 1 && c == '0',
              "read after SEEK_SET got '%c'", c);
        CHECK(fd_seek(1, f, -1, SEEK_CUR) == 0,
              "SEEK_CUR -1 from 1 failed");
        int pos = fd_seek(1, f, -3, SEEK_END);
        CHECK(pos == 7, "SEEK_END -3 = %d, expected 7", pos);
        CHECK(fd_read(1, f, &c, 1) == 1 && c == '7',
              "read after SEEK_END got '%c'", c);
        CHECK(fd_seek(1, f, 10, SEEK_SET) == 10,
              "SEEK_SET to EOF was rejected");
        CHECK(fd_seek(1, f, 11, SEEK_SET) == -1,
              "SEEK_SET past EOF was accepted");
        CHECK(fd_seek(1, f, -11, SEEK_CUR) == -1,
              "SEEK_CUR before start was accepted");
        CHECK(fd_seek(1, f, 1, SEEK_END) == -1,
              "SEEK_END past EOF was accepted");
        CHECK(fd_seek(1, f, -1, SEEK_SET) == -1,
              "SEEK_SET before start was accepted");
        pos = fd_seek(1, f, 0, SEEK_CUR);
        CHECK(pos == 10, "failed seek moved offset, SEEK_CUR = %d", pos);
        CHECK(fd_seek(1, f, 0, SEEK_SET) == 0, "final rewind failed");
        CHECK(fd_read(1, f, &c, 1) == 1 && c == '0',
              "final read got '%c'", c);
    }

    if (f >= 0) CHECK(fd_close(1, f) == 0, "close failed");
    CHECK(fd_stub_total_refs() == 0, "slot refs after close = %d",
          fd_stub_total_refs());
    fd_close_all(&processes[1]);
}

static void test_clone_process(void) {
    printf("\n=== Test 7: clone process shares OFDs ===\n");
    fd_stub_reset();
    fd_init_process(&processes[1]);
    fd_init_process(&processes[2]);

    int a = fd_open(1, "clone", O_RDWR | O_CREAT);
    CHECK(a >= 0, "open \"clone\" failed");
    if (a >= 0) {
        CHECK(fd_write(1, a, "hi", 2) == 2, "write failed");
        CHECK(fd_seek(1, a, 0, SEEK_SET) == 0, "seek to 0 failed");

        unsigned before = fd_stub_add_ref_calls();
        CHECK(fd_clone_process(&processes[1], &processes[2]) == 0,
              "fd_clone_process failed");
        CHECK(fd_stub_add_ref_calls() - before == 1,
              "clone called fs_slot_add_ref %u times",
              fd_stub_add_ref_calls() - before);
        CHECK(processes[2].fds[a].kind == FD_ENTRY_FILE,
              "child fd %d kind = %d", a, processes[2].fds[a].kind);
        CHECK(processes[2].fds[a].ofd == processes[1].fds[a].ofd,
              "child OFD %d, parent OFD %d", processes[2].fds[a].ofd,
              processes[1].fds[a].ofd);
        CHECK(processes[2].fds[FD_STDOUT].kind == FD_ENTRY_CONSOLE_OUT,
              "child stdout kind = %d", processes[2].fds[FD_STDOUT].kind);

        char c = 0;
        CHECK(fd_read(1, a, &c, 1) == 1 && c == 'h',
              "parent read got '%c'", c);
        CHECK(fd_read(2, a, &c, 1) == 1 && c == 'i',
              "child read got '%c'", c);

        CHECK(fd_close(2, a) == 0, "child close failed");
        CHECK(fd_stub_total_refs() == 1, "slot refs after child close = %d",
              fd_stub_total_refs());
        CHECK(fd_seek(1, a, 0, SEEK_SET) == 0, "parent seek after child close failed");
        CHECK(fd_read(1, a, &c, 1) == 1 && c == 'h',
              "parent read after child close got '%c'", c);
        CHECK(fd_close(1, a) == 0, "parent close failed");
        CHECK(fd_stub_total_refs() == 0, "slot refs after both closes = %d",
              fd_stub_total_refs());
    }

    fd_close_all(&processes[1]);
    fd_close_all(&processes[2]);
}

static void test_pool_exhaustion(void) {
    printf("\n=== Test 8: fd and OFD pool exhaustion ===\n");
    fd_stub_reset();
    for (int pid = 1; pid <= 3; pid++) fd_init_process(&processes[pid]);

    /* Fd 0/1/2 are reusable, so close them to get 16 slots per process. */
    for (int pid = 1; pid <= 2; pid++) {
        for (int fd = 0; fd < 3; fd++) {
            CHECK(fd_close(pid, fd) == 0, "close std fd %d failed", fd);
        }
    }

    char name[16];
    for (int pid = 1; pid <= 2; pid++) {
        for (int i = 0; i < FD_MAX; i++) {
            snprintf(name, sizeof(name), "p%d-%d", pid, i);
            int fd = fd_open(pid, name, O_RDWR | O_CREAT);
            CHECK(fd == i, "pid %d open %d returned %d", pid, i, fd);
        }
    }
    CHECK(fd_stub_total_refs() == OFD_MAX, "filled slot refs = %d, expected %d",
          fd_stub_total_refs(), OFD_MAX);

    int before = fd_stub_total_refs();
    CHECK(fd_open(1, "p1-0", O_RDWR) == -1,
          "open with a full fd table was accepted");
    CHECK(fd_stub_total_refs() == before,
          "fd exhaustion leaked refs (%d -> %d)", before,
          fd_stub_total_refs());

    for (int fd = 0; fd < 3; fd++) {
        CHECK(fd_close(3, fd) == 0, "close pid 3 std fd %d failed", fd);
    }

    before = fd_stub_total_refs();
    CHECK(fd_open(3, "p1-0", O_RDWR) == -1,
          "open with a full OFD pool was accepted");
    CHECK(fd_stub_total_refs() == before,
          "OFD exhaustion leaked refs (%d -> %d)", before,
          fd_stub_total_refs());

    for (int pid = 1; pid <= 3; pid++) fd_close_all(&processes[pid]);
    CHECK(fd_stub_total_refs() == 0, "slot refs after close-all = %d",
          fd_stub_total_refs());
}

int main(void) {
    printf("ARC OS file descriptor tests\n");
    test_std_fds();
    test_independent_offsets();
    test_dup_shares_offset();
    test_close_std_fd_reuses_number();
    test_access_flags();
    test_seek_bounds();
    test_clone_process();
    test_pool_exhaustion();

    printf("\n=== fd suite: %d checks, %d failures ===\n",
           tests_passed, tests_failed);
    return tests_failed ? 1 : 0;
}
