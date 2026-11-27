/*
 * ARC OS Userspace Shell (ring 3)
 * All kernel access goes through the int 0x80 syscall gate.
 */

/* Syscall numbers (must match kernel.h / syscall.c) */
#define SYS_WRITE 1
#define SYS_READ  2
#define SYS_EXIT  3
#define SYS_OPEN  4
#define SYS_CLOSE 5
#define SYS_LS    6
#define SYS_DEL   7
#define SYS_SEEK  8
#define SYS_SIZE  9
#define SYS_TRUNC 10
#define SYS_RENAME 11
#define SYS_CLEAR 12
#define SYS_FORK 14
#define SYS_WAIT 15
#define SYS_EXEC 16
#define SYS_SYNC 22
#ifdef ARC_RING0_FAULT_TEST
#define SYS_RING0_FAULT_TEST 24
#endif
#ifdef ARC_DOUBLE_FAULT_TEST
#define SYS_DOUBLE_FAULT_TEST 25
#endif
#ifdef ARC_PANIC_DIAG_TEST
#define SYS_PANIC_UD_TEST 26
#define SYS_PANIC_PF_TEST 27
#define SYS_PANIC_RECURSIVE_TEST 28
#endif

/* Open flags (must match kernel.h) */
#define O_RDONLY 0x0000
#define O_WRONLY 0x0001
#define O_CREAT  0x0100
#define O_TRUNC  0x0200

#define STDIN  0
#define STDOUT 1
#define FS_FD0 3

/* Substring check for command dispatch. */
static int starts_with(const char* s, const char* prefix) {
    const char* a = s;
    const char* b = prefix;
    while (*b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return 1;
}

/* Copy the next whitespace-delimited token from src into out; returns out. */
static const char* next_token(const char* src, char* out, long outsz) {
    while (*src == ' ') src++;
    long i = 0;
    while (*src && *src != ' ' && *src != '>' && i < outsz - 1) {
        out[i++] = *src++;
    }
    out[i] = '\0';
    return out;
}

/* int 0x80 ABI: rdi = number, rsi/rdx/rcx = a1..a3 (SysV arg slots), rax = return. */
static long syscall4(long n, long a1, long a2, long a3) {
    long ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "D"(n), "S"(a1), "d"(a2), "c"(a3)
                     : "rbx", "memory");
    return ret;
}

static long sys_fork(void) { return syscall4(14, 0, 0, 0); }
static long sys_wait(int* s) { return syscall4(15, (long)s, 0, 0); }
static long sys_exec(const char* path, char* const argv[]) {
    return syscall4(SYS_EXEC, (long)path, (long)argv, 0);
}

static void print_str(const char* s) {
    long len = 0;
    while (s[len]) len++;
    syscall4(SYS_WRITE, STDOUT, (long)s, len);
}

static void print_newline(void) {
    syscall4(SYS_WRITE, 1, (long)"\n", 1);
}

static void print_prompt(void) {
    syscall4(SYS_WRITE, 1, (long)"arc> ", 5);
}

#if defined(ARC_SYSCALL_TEST) || defined(ARC_EXCEPTION_TEST)
/* Test-only integer printer; the normal shell image cannot reach this code. */
static void print_test_long(long value) {
    char buf[24];
    int pos = (int)sizeof(buf);
    unsigned long magnitude = value < 0
        ? (unsigned long)(-(value + 1)) + 1UL
        : (unsigned long)value;

    buf[--pos] = '\0';
    if (magnitude == 0) buf[--pos] = '0';
    while (magnitude) {
        buf[--pos] = (char)('0' + (magnitude % 10UL));
        magnitude /= 10UL;
    }
    if (value < 0) buf[--pos] = '-';
    syscall4(SYS_WRITE, STDOUT, (long)&buf[pos],
             (long)(sizeof(buf) - (unsigned long)pos - 1UL));
}
#endif

#ifdef ARC_EXCEPTION_TEST
/* Test-only ring-3 fault triggers. Each must trap into the isolation path. */
static void trigger_user_fault(int kind) {
    if (kind == 0) {  /* #DE: divide EDX:EAX by zero */
        __asm__ volatile("xorl %%edx, %%edx\n"
                         "movl $1, %%eax\n"
                         "xorl %%ecx, %%ecx\n"
                         "idivl %%ecx\n"
                         : : : "rax", "rcx", "rdx", "cc");
    } else if (kind == 1) {  /* #UD: architecturally invalid opcode */
        __asm__ volatile("ud2" : : : "memory");
    } else {  /* #PF: write to an unmapped canonical user address */
        unsigned long bad = 0x00000000B00B0000UL;
        __asm__ volatile("movq $0, (%0)" : : "r"(bad) : "memory");
    }
}

static void run_fault_case(const char* tag, int kind, long expected_status) {
    print_str("USERFAULT ");
    print_str(tag);
    print_str(" begin\n");

    long pid = sys_fork();
    if (pid == 0) {
        print_str("USERFAULT ");
        print_str(tag);
        print_str(" child faulting\n");
        trigger_user_fault(kind);
        /* The kernel must never let the child resume here. */
        syscall4(SYS_EXIT, 99, 0, 0);
        return;
    }

    int status = -1;
    long got = sys_wait(&status);
    print_str("USERFAULT ");
    print_str(tag);
    print_str(" wait ");
    print_test_long(got);
    print_newline();
    print_str("USERFAULT ");
    print_str(tag);
    print_str(" status ");
    print_test_long(status);
    print_newline();
    if (got > 0 && status == expected_status) {
        print_str("USERFAULT ");
        print_str(tag);
        print_str(" isolated\n");
    } else {
        print_str("USERFAULT ");
        print_str(tag);
        print_str(" bad status\n");
    }
    print_str("USERFAULT ");
    print_str(tag);
    print_str(" shell alive\n");
}
#endif

static void run_command(const char* cmd) {
    char tok[80];

    if (starts_with(cmd, "help")) {
        print_str("ARC OS Commands:\n");
        print_str("  help                - Show this message\n");
        print_str("  clear               - Clear screen\n");
        print_str("  ls                  - List files (with sizes)\n");
        print_str("  echo text           - Print text\n");
        print_str("  echo text to file    - Write text to a file\n");
        print_str("  cat <file>          - Print a file\n");
        print_str("  touch <file>        - Create/update a file\n");
        print_str("  cp <src> <dst>      - Copy a file\n");
        print_str("  mv <src> <dst>      - Move/rename a file\n");
        print_str("  truncate <file>     - Empty a file\n");
        print_str("  rm <file>           - Delete a file\n");
        print_str("  sync                - Flush filesystem to disk\n");
        print_str("  forktest            - Prove fork/wait with a child\n");
        print_str("  execdemo            - Prove fork -> exec -> wait with hello.bin\n");
        print_str("  exit                - Exit shell\n");
        return;
    }

    if (starts_with(cmd, "clear")) {
        syscall4(SYS_CLEAR, 0, 0, 0);
        return;
    }

    if (starts_with(cmd, "forktest")) {
        print_str("fork: parent\n");
        long pid = sys_fork();
        if (pid == 0) {
            print_str("fork: child ok\n");
            syscall4(SYS_EXIT, 7, 0, 0);
            return;
        }
        int status = -1;
        sys_wait(&status);
        if (status == 7)
            print_str("fork: wait ok\n");
        else
            print_str("fork: wait bad\n");
        return;
    }

    if (starts_with(cmd, "execdemo")) {
        print_str("exec: parent\n");
        long pid = sys_fork();
        if (pid == 0) {
            char* argv[] = {"hello.bin", "alpha", 0};
            sys_exec("hello.bin", argv);
            print_str("exec: failed\n");
            syscall4(SYS_EXIT, 9, 0, 0);
            return;
        }
        int status = -1;
        sys_wait(&status);
        if (status == 0)
            print_str("exec: wait ok\n");
        else
            print_str("exec: wait bad\n");
        return;
    }

    if (starts_with(cmd, "ls")) {
        syscall4(SYS_LS, 0, 0, 0);
        return;
    }

    if (starts_with(cmd, "echo")) {
        /* echo can either print text or write it to a file.
         * Redirect syntax:  echo text > <file>   (unix)
         *              or:  echo text to <file>  (convenience, no '>') */
        const char* fname = 0;
        char* textend = 0;

        const char* gt = cmd + 4;
        while (*gt && *gt != '>') gt++;
        if (*gt == '>') {
            textend = (char*)gt;
            fname = gt + 1;
            while (*fname == ' ') fname++;
        } else {
            /* look for a lone " to " word separator */
            const char* p = cmd + 4;
            while (*p) {
                if (p[0] == ' ' && p[1] == 't' && p[2] == 'o' &&
                    (p[3] == '\0' || p[3] == ' ')) {
                    textend = (char*)p;
                    fname = p + 3;
                    while (*fname == ' ') fname++;
                    break;
                }
                p++;
            }
        }

        if (fname != 0 && *fname != '\0') {
            char text[128];
            long ti = 0;
            const char* s = cmd + 4;
            while (*s == ' ') s++;
            while (s < textend) {
                if (ti >= (long)sizeof(text) - 1) break;
                text[ti++] = *s++;
            }
            while (ti > 0 && (text[ti - 1] == ' ')) ti--;
            text[ti] = '\0';

            long fd = syscall4(SYS_OPEN, (long)fname,
                               O_WRONLY | O_CREAT | O_TRUNC, 0);
            if (fd < FS_FD0) {
                print_str("echo: cannot create ");
                print_str(fname);
                print_newline();
                return;
            }
            syscall4(SYS_WRITE, fd, (long)&text[0], ti);
            syscall4(SYS_CLOSE, fd, 0, 0);
            print_str(fname);
            print_str(" written\n");
        } else {
            const char* s = cmd + 4;
            while (*s == ' ') s++;
            print_str(s);
            print_newline();
        }
        return;
    }

    if (starts_with(cmd, "cat")) {
        const char* fname = next_token(cmd + 3, tok, sizeof(tok));
        if (tok[0] == '\0') {
            print_str("usage: cat <file>\n");
            return;
        }
        long fd = syscall4(SYS_OPEN, (long)fname, O_RDONLY, 0);
        if (fd < FS_FD0) {
            print_str("cat: no such file: ");
            print_str(fname);
            print_newline();
            return;
        }
        char chunk[128];
        char last = '\n';
        for (;;) {
            long n = syscall4(SYS_READ, fd, (long)&chunk[0], (long)sizeof(chunk));
            if (n <= 0) break;
            last = chunk[n - 1];
            long k = 0;
            while (k < n) {
                long w = syscall4(SYS_WRITE, STDOUT, (long)&chunk[k], n - k);
                if (w <= 0) break;
                k += w;
            }
        }
        syscall4(SYS_CLOSE, fd, 0, 0);
        if (last != '\n') print_newline();
        return;
    }

    if (starts_with(cmd, "rm")) {
        const char* fname = next_token(cmd + 2, tok, sizeof(tok));
        if (tok[0] == '\0') {
            print_str("usage: rm <file>\n");
            return;
        }
        long r = syscall4(SYS_DEL, (long)fname, 0, 0);
        if (r < 0) {
            print_str("rm: no such file: ");
            print_str(fname);
            print_newline();
        }
        return;
    }

    if (starts_with(cmd, "touch")) {
        const char* fname = next_token(cmd + 5, tok, sizeof(tok));
        if (tok[0] == '\0') {
            print_str("usage: touch <file>\n");
            return;
        }
        long fd = syscall4(SYS_OPEN, (long)fname, O_WRONLY | O_CREAT, 0);
        if (fd < FS_FD0) {
            print_str("touch: cannot create ");
            print_str(fname);
            print_newline();
            return;
        }
        syscall4(SYS_CLOSE, fd, 0, 0);
        return;
    }

    if (starts_with(cmd, "truncate")) {
        const char* fname = next_token(cmd + 8, tok, sizeof(tok));
        if (tok[0] == '\0') {
            print_str("usage: truncate <file>\n");
            return;
        }
        long fd = syscall4(SYS_OPEN, (long)fname, O_WRONLY, 0);
        if (fd < FS_FD0) {
            print_str("truncate: no such file: ");
            print_str(fname);
            print_newline();
            return;
        }
        syscall4(SYS_TRUNC, fd, 0, 0);
        syscall4(SYS_CLOSE, fd, 0, 0);
        return;
    }

    if (starts_with(cmd, "sync")) {
        long r = syscall4(SYS_SYNC, 0, 0, 0);
        if (r == 0) print_str("sync: 0\n");
        else print_str("sync: -1\n");
        return;
    }

    if (starts_with(cmd, "cp")) {
        /* parse two whitespace-separated names from "cp <src> <dst>" */
        const char* p = cmd + 2;
        while (*p == ' ') p++;
        const char* s1 = p;
        while (*p && *p != ' ' && *p != '>') p++;
        long s1len = p - s1;
        while (*p == ' ') p++;
        const char* s2 = p;
        while (*p && *p != ' ' && *p != '>') p++;
        long s2len = p - s2;
        if (s1len == 0 || s2len == 0) {
            print_str("usage: cp <src> <dst>\n");
            return;
        }
        char src[80], dst[80];
        if (s1len >= (long)sizeof(src)) s1len = sizeof(src) - 1;
        if (s2len >= (long)sizeof(dst)) s2len = sizeof(dst) - 1;
        for (long i = 0; i < s1len; i++) src[i] = s1[i];
        src[s1len] = '\0';
        for (long i = 0; i < s2len; i++) dst[i] = s2[i];
        dst[s2len] = '\0';

        long sr = syscall4(SYS_OPEN, (long)src, O_RDONLY, 0);
        if (sr < FS_FD0) {
            print_str("cp: no such file: ");
            print_str(src);
            print_newline();
            return;
        }
        long dr = syscall4(SYS_OPEN, (long)dst,
                           O_WRONLY | O_CREAT | O_TRUNC, 0);
        if (dr < FS_FD0) {
            print_str("cp: cannot create ");
            print_str(dst);
            print_newline();
            syscall4(SYS_CLOSE, sr, 0, 0);
            return;
        }
        char cp_buf[256];
        for (;;) {
            long n = syscall4(SYS_READ, sr, (long)&cp_buf[0], (long)sizeof(cp_buf));
            if (n <= 0) break;
            long off = 0;
            while (off < n) {
                long w = syscall4(SYS_WRITE, dr, (long)&cp_buf[off], n - off);
                if (w <= 0) break;
                off += w;
            }
        }
        syscall4(SYS_CLOSE, sr, 0, 0);
        syscall4(SYS_CLOSE, dr, 0, 0);
        print_str("cp: ");
        print_str(src);
        print_str(" -> ");
        print_str(dst);
        print_newline();
        return;
    }

    if (starts_with(cmd, "mv")) {
        const char* p = cmd + 2;
        while (*p == ' ') p++;
        const char* s1 = p;
        while (*p && *p != ' ' && *p != '>') p++;
        long s1len = p - s1;
        while (*p == ' ') p++;
        const char* s2 = p;
        while (*p && *p != ' ' && *p != '>') p++;
        long s2len = p - s2;
        if (s1len == 0 || s2len == 0) {
            print_str("usage: mv <src> <dst>\n");
            return;
        }
        char src[80], dst[80];
        if (s1len >= (long)sizeof(src)) s1len = sizeof(src) - 1;
        if (s2len >= (long)sizeof(dst)) s2len = sizeof(dst) - 1;
        for (long i = 0; i < s1len; i++) src[i] = s1[i];
        src[s1len] = '\0';
        for (long i = 0; i < s2len; i++) dst[i] = s2[i];
        dst[s2len] = '\0';

        long r = syscall4(SYS_RENAME, (long)src, (long)dst, 0);
        if (r < 0) {
            print_str("mv: cannot rename ");
            print_str(src);
            print_str(" to ");
            print_str(dst);
            print_newline();
            return;
        }
        print_str("mv: ");
        print_str(src);
        print_str(" -> ");
        print_str(dst);
        print_newline();
        return;
    }

#ifdef ARC_SYSCALL_TEST
    if (starts_with(cmd, "ptrtest")) {
        /* Deliberately pass a kernel address as a user buffer. The gate checks
         * that both syscalls reject it and that this shell keeps running. */
        void* kernel_va = (void*)0x200000UL;
        long written, read;

        print_str("SYSCALL PTR TEST\n");
        written = syscall4(SYS_WRITE, STDOUT, (long)kernel_va, 4);
        print_str("write ");
        print_test_long(written);
        print_newline();

        read = syscall4(SYS_READ, STDIN, (long)kernel_va, 4);
        print_str("read ");
        print_test_long(read);
        print_newline();
        print_str("shell alive\n");
        return;
    }

    if (starts_with(cmd, "readok")) {
        /* Positive case: a real ring-3 stack buffer handed to SYS_READ.
         * Request the whole payload in one call so the kernel copies more
         * than one byte through copy_to_user(); fd_read() can still return a
         * short count when the UART ring drains in pieces, so loop only on
         * those short returns. SYS_WRITE echoes the bytes back through
         * copy_from_user(). */
        char buf[8];
        long got = 0;
        long largest = 0;

        print_str("SYSCALL READ OK TEST\n");
        while (got < 6) {
            long r = syscall4(SYS_READ, STDIN, (long)&buf[got], 6 - got);
            if (r <= 0) continue;
            if (r > largest) largest = r;
            got += r;
        }
        buf[got] = '\0';

        print_str("read ");
        print_test_long(got);
        print_newline();
        print_str("bulk ");
        print_test_long(largest);
        print_newline();
        if (largest > 1) print_str("bulk copy ok\n");
        print_str("got ");
        syscall4(SYS_WRITE, STDOUT, (long)buf, got);
        print_newline();
        print_str("shell alive\n");
        return;
    }
#endif

#ifdef ARC_RING0_FAULT_TEST
    if (starts_with(cmd, "ring0fault")) {
        print_str("RING0FAULT invoked from ring 3\n");
        syscall4(SYS_RING0_FAULT_TEST, 0, 0, 0);
        return;
    }
#endif

#ifdef ARC_PANIC_DIAG_TEST
    if (starts_with(cmd, "panicud")) {
        print_str("PANICUD invoked from ring 3\n");
        syscall4(SYS_PANIC_UD_TEST, 0, 0, 0);
        return;
    }
    if (starts_with(cmd, "panicpf")) {
        print_str("PANICPF invoked from ring 3\n");
        syscall4(SYS_PANIC_PF_TEST, 0, 0, 0);
        return;
    }
    if (starts_with(cmd, "panicrecursive")) {
        print_str("PANICREC invoked from ring 3\n");
        syscall4(SYS_PANIC_RECURSIVE_TEST, 0, 0, 0);
        return;
    }
#endif

#ifdef ARC_DOUBLE_FAULT_TEST
    if (starts_with(cmd, "doublefault")) {
        print_str("DOUBLEFAULT invoked from ring 3\n");
        syscall4(SYS_DOUBLE_FAULT_TEST, 0, 0, 0);
        return;
    }
#endif

#ifdef ARC_EXCEPTION_TEST
    if (starts_with(cmd, "faultde")) {
        run_fault_case("DE", 0, 128);
        return;
    }
    if (starts_with(cmd, "faultud")) {
        run_fault_case("UD", 1, 134);
        return;
    }
    if (starts_with(cmd, "faultpf")) {
        run_fault_case("PF", 2, 142);
        return;
    }
#endif

    if (starts_with(cmd, "exit")) {
        syscall4(SYS_EXIT, 0, 0, 0);
        return;
    }

    print_str("Unknown command: ");
    print_str(cmd);
    print_newline();
    print_str("Type 'help' for available commands.\n");
}

static void read_line(char* buf, long max_len) {
    long pos = 0;
    while (1) {
        char c;
        long ret = syscall4(SYS_READ, 0, (long)&c, 1);
        if (ret <= 0) continue;

        if (c == '\n') {
            buf[pos] = '\0';
            print_newline();
            break;
        }
        if (c == '\b') {
            if (pos > 0) {
                pos--;
                syscall4(SYS_WRITE, 1, (long)"\b \b", 3);
            }
            continue;
        }
        if (pos < max_len - 1) {
            buf[pos++] = c;
            syscall4(SYS_WRITE, 1, (long)&c, 1);
        }
    }
}

void shell_main(void) {
    print_str("=== ARC OS Shell v1.0 (ring 3) ===\n");
    print_str("Type 'help' for available commands.\n\n");

    for (;;) {
        print_prompt();

        char buf[256];
        read_line(buf, (long)sizeof(buf));

        if (buf[0] == '\0') continue;
        run_command(buf);
    }
}

/* User entry ABI shared with the exec'd programs: crt0 passes argc/argv. */
void user_main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    shell_main();
}