#include "kernel.h"

process_t processes[MAX_PROCESSES];
int current_pid = 0;
int num_processes = 0;
int current_process = -1;

void scheduler_init(void) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        processes[i].pid = i;
        processes[i].running = 0;
        processes[i].rsp = 0;
        processes[i].rip = 0;
    }
}

void process_create(void (*entry)(void), const char* name) {
    if (num_processes >= MAX_PROCESSES) return;
    process_t* p = &processes[num_processes];
    p->pid = num_processes;
    p->running = 1;
    p->rsp = USER_STACK_TOP - (num_processes * 0x10000);
    p->rip = (uint64_t)entry;
    p->name[0] = '\0';
    int i = 0;
    while (name[i] && i < 31) {
        p->name[i] = name[i];
        i++;
    }
    p->name[i] = '\0';
    num_processes++;
}

void scheduler_yield(void) {
    if (num_processes == 0) return;
    current_process = (current_process + 1) % num_processes;
}
