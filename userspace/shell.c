/*
 * ARC OS Userspace Shell
 * Simple command-line interpreter
 */
#include <stddef.h>

/* Syscall numbers */
#define SYS_WRITE   1
#define SYS_READ    2
#define SYS_EXIT    3
#define SYS_OPEN    4
#define SYS_CLOSE   5
#define SYS_LS      6
#define SYS_HELP    7
#define SYS_CLEAR   8

extern int syscall_write(int fd, const char* buf, size_t count);
extern int syscall_read(int fd, char* buf, size_t count);
extern void syscall_exit(int code);

static void print_prompt(void) {
    syscall_write(1, "arc> ", 5);
}

static void print_str(const char* s) {
    int len = 0;
    while (s[len]) len++;
    syscall_write(1, s, len);
}

static void print_newline(void) {
    syscall_write(1, "\n", 1);
}

/* Handle a command string */
static void run_command(const char* cmd) {
    /* Check for built-in commands */
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
        syscall_exit(0);
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
        /* Print everything after "echo " */
        const char* text = cmd + 5;
        print_str(text);
        print_newline();
        return;
    }
    if (cmd[0] == 'e' && cmd[1] == 'x' && cmd[2] == 'i' && cmd[3] == 't') {
        syscall_exit(0);
        return;
    }
    
    /* Unknown command */
    print_str("Unknown command: ");
    print_str(cmd);
    print_newline();
    print_str("Type 'help' for available commands.\n");
}

/* Simple line reader */
static void read_line(char* buf, int max_len) {
    int pos = 0;
    while (1) {
        char c;
        int ret = syscall_read(0, &c, 1);
        if (ret <= 0) continue;
        
        if (c == '\n') {
            buf[pos] = '\0';
            print_newline();
            break;
        }
        if (c == '\b') {
            if (pos > 0) {
                pos--;
                syscall_write(1, "\b \b", 3);
            }
            continue;
        }
        if (pos < max_len - 1) {
            buf[pos++] = c;
            syscall_write(1, &c, 1);
        }
    }
}

void shell_main(void) {
    print_str("=== ARC OS Shell v1.0 ===\n");
    print_str("Type 'help' for available commands.\n\n");
    
    while (1) {
        print_prompt();
        
        char buf[256];
        read_line(buf, sizeof(buf));
        
        if (buf[0] == '\0') continue;
        
        run_command(buf);
    }
}
