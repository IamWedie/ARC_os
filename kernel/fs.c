#include "kernel.h"

static fs_t filesystem;

/* Simple in-memory filesystem */
/* Each file is stored in a fixed location in memory */

static uint8_t file_data[FS_MAX_FILES][FS_MAX_FILESIZE];
static int file_data_used[FS_MAX_FILES];

void fs_init(void) {
    filesystem.num_files = 0;
    filesystem.initialized = 1;
    
    for (int i = 0; i < FS_MAX_FILES; i++) {
        filesystem.files[i].used = 0;
        filesystem.files[i].name[0] = '\0';
        filesystem.files[i].size = 0;
        filesystem.files[i].start_sector = 0;
        file_data_used[i] = 0;
    }
    
    /* Register some built-in files */
    /* These are stored in the kernel binary itself */
    filesystem.files[0].used = 1;
    filesystem.files[0].name[0] = 'k'; filesystem.files[0].name[1] = 'e';
    filesystem.files[0].name[2] = 'r'; filesystem.files[0].name[3] = 'n';
    filesystem.files[0].name[4] = 'e'; filesystem.files[0].name[5] = 'l';
    filesystem.files[0].name[6] = '.'; filesystem.files[0].name[7] = 'c';
    filesystem.files[0].name[8] = 'o'; filesystem.files[0].name[9] = 'd';
    filesystem.files[0].name[10] = 'e'; filesystem.files[0].name[11] = '\0';
    filesystem.files[0].size = 0;
    filesystem.files[0].start_sector = 0;
    filesystem.num_files = 1;
    
    filesystem.files[1].used = 1;
    filesystem.files[1].name[0] = 's'; filesystem.files[1].name[1] = 'h';
    filesystem.files[1].name[2] = 'e'; filesystem.files[1].name[3] = 'l';
    filesystem.files[1].name[4] = 'l'; filesystem.files[1].name[5] = '.';
    filesystem.files[1].name[6] = 'c'; filesystem.files[1].name[7] = '\0';
    filesystem.files[1].size = 0;
    filesystem.files[1].start_sector = 0;
    filesystem.num_files = 2;
}

int fs_open(const char* name) {
    if (!filesystem.initialized) return -1;
    
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (filesystem.files[i].used) {
            int match = 1;
            const char* fn = filesystem.files[i].name;
            const char* n = name;
            while (*fn && *n) {
                if (*fn != *n) { match = 0; break; }
                fn++; n++;
            }
            if (*fn == '\0' && *n == '\0' && match) {
                return i;
            }
        }
    }
    return -1;
}

int fs_read(int fd, void* buf, size_t count) {
    if (fd < 0 || fd >= FS_MAX_FILES) return -1;
    if (!filesystem.files[fd].used) return -1;
    return 0; /* Read returns 0 (no more data) */
}

int fs_close(int fd) {
    return 0;
}

int fs_list(void) {
    if (!filesystem.initialized) return -1;
    
    terminal_putstring("Files:\n");
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (filesystem.files[i].used) {
            terminal_putstring("  ");
            terminal_putstring(filesystem.files[i].name);
            terminal_putstring("\n");
        }
    }
    return 0;
}
