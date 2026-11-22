/*
 * ARC OS ring-3 exec proof program (freestanding, no libc).
 * The kernel-built argument stack must deliver argv[1] == "alpha".
 */

/* Syscall numbers (must match kernel.h / syscall.c) */
#define SYS_WRITE 1
#define SYS_EXIT  3

#define STDOUT 1

/*
 * SYS_EXEC enters the image at USER_CODE_VA, so crt0's _start must stay at
 * blob offset 0. Keep all code and literals in .text: crt0.o is linked first,
 * and large-code-model clang would otherwise emit orphan .ltext/.lrodata
 * sections ahead of the .text startup stub.
 */
#define IN_START_TEXT __attribute__((section(".text")))
/* .text.rodata still matches link.ld's *(.text*) but keeps data/code section
 * types separate (clang rejects mixing them in one section). */
#define IN_TEXT_DATA __attribute__((section(".text.rodata")))

IN_TEXT_DATA static const char MSG_OK[]    = "exec: hello ok\n";
IN_TEXT_DATA static const char MSG_BAD[]   = "exec: hello bad\n";
IN_TEXT_DATA static const char ARG_ALPHA[] = "alpha";

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

static int str_eq(const char* a, const char* b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* User entry ABI shared with the shell: crt0 passes argc/argv. */
IN_START_TEXT
void user_main(int argc, char** argv) {
    if (argc == 2 && argv && argv[1] && str_eq(argv[1], ARG_ALPHA))
        print_str(MSG_OK);
    else
        print_str(MSG_BAD);
    syscall4(SYS_EXIT, 0, 0, 0);
}
