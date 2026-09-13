/*
 * ARC OS Userspace Shell (ring 3)
 * All kernel access goes through the int 0x80 syscall gate.
 */

/* Syscall numbers (must match kernel.h / syscall.c) */
#define SYS_WRITE 1
#define SYS_READ  2
#define SYS_EXIT  3

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
    syscall4(SYS_WRITE, 1, (long)s, len);
}

static void print_newline(void) {
    syscall4(SYS_WRITE, 1, (long)"\n", 1);
}

static void print_prompt(void) {
    syscall4(SYS_WRITE, 1, (long)"arc> ", 5);
}

static void run_command(const char* cmd) {
    if (cmd[0] == 'h' && cmd[1] == 'e' && cmd[2] == 'l' && cmd[3] == 'p') {
        print_str("ARC OS Commands:\n");
        print_str("  help    - Show this message\n");
        print_str("  clear   - Clear screen\n");
        print_str("  ls      - List files\n");
        print_str("  echo    - Print text\n");
        print_str("  exit    - Exit shell\n");
        return;
    }
    if (cmd[0] == 'c' && cmd[1] == 'l' && cmd[2] == 'e' && cmd[3] == 'a' && cmd[4] == 'r') {
        syscall4(SYS_EXIT, 0, 0, 0);
        return;
    }
    if (cmd[0] == 'l' && cmd[1] == 's') {
        print_str("ARC OS filesystem (v0.1)\n");
        print_str("  /kernel\n");
        print_str("  /shell\n");
        print_str("  /lib\n");
        return;
    }
    if (cmd[0] == 'e' && cmd[1] == 'c' && cmd[2] == 'h' && cmd[3] == 'o') {
        print_str(cmd + 5);
        print_newline();
        return;
    }
    if (cmd[0] == 'e' && cmd[1] == 'x' && cmd[2] == 'i' && cmd[3] == 't') {
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