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

static void run_command(const char* cmd) {
    char tok[80];

    if (starts_with(cmd, "help")) {
        print_str("ARC OS Commands:\n");
        print_str("  help                - Show this message\n");
        print_str("  clear               - Clear screen\n");
        print_str("  ls                  - List files\n");
        print_str("  echo text           - Print text\n");
        print_str("  echo text to file    - Write text to a file\n");
        print_str("  cat <file>          - Print a file\n");
        print_str("  rm <file>           - Delete a file\n");
        print_str("  exit                - Exit shell\n");
        return;
    }

    if (starts_with(cmd, "clear")) {
        syscall4(SYS_EXIT, 0, 0, 0);
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

            long fd = syscall4(SYS_OPEN, (long)fname, 1, 0);
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
        long fd = syscall4(SYS_OPEN, (long)fname, 0, 0);
        if (fd < FS_FD0) {
            print_str("cat: no such file: ");
            print_str(fname);
            print_newline();
            return;
        }
        char chunk[128];
        for (;;) {
            long n = syscall4(SYS_READ, fd, (long)&chunk[0], (long)sizeof(chunk));
            if (n <= 0) break;
            long k = 0;
            while (k < n) {
                long w = syscall4(SYS_WRITE, STDOUT, (long)&chunk[k], n - k);
                if (w <= 0) break;
                k += w;
            }
        }
        syscall4(SYS_CLOSE, fd, 0, 0);
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