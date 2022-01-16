#include <stdbool.h>

#include "console.h"
#include "memory.h"
#include "process.h"

process_t* processes;
size_t processes_list_page_count;

pid_t MAX_PID = 1024;
pid_t current_pid = 0;

// init_processes() -> void
// Initialises process related stuff.
void init_processes() {
    processes_list_page_count = (MAX_PID * sizeof(process_t) + PAGE_SIZE - 1) / PAGE_SIZE;
    processes = alloc_pages(processes_list_page_count);
    mmu_level_1_t* top = get_mmu();
    for (size_t i = 0; i < processes_list_page_count; i++) {
        mmu_change_flags(top, (void*) processes + i * PAGE_SIZE, MMU_BIT_READ | MMU_BIT_WRITE | MMU_BIT_GLOBAL);
    }
}

// spawn_process_from_elf(pid_t, elf_t*, size_t, void*, size_t) -> pid_t
// Spawns a process using the given elf file and parent pid. Returns -1 on failure.
pid_t spawn_process_from_elf(pid_t parent_pid, elf_t* elf, size_t stack_size, void* args, size_t arg_size) {
    if (elf->header->type != ELF_EXECUTABLE) {
        console_puts("ELF file is not executable\n");
        return -1;
    }

    pid_t pid = -1;
    if (current_pid < MAX_PID) {
        pid = current_pid++;
    } else {
        for (size_t i = 0; i < MAX_PID; i++) {
            if (processes[i].state == PROCESS_STATE_DEAD) {
                pid = i;
                break;
            }
        }

        if (pid == (uint64_t) -1)
            return -1;
    }

    mmu_level_1_t* top;
    if (current_pid == 1) {
        top = get_mmu();
    } else {
        top = create_mmu_table();
        identity_map_kernel(top, NULL, NULL, NULL);
        for (size_t i = 0; i < processes_list_page_count; i++) {
            void* p = (void*) processes + i * PAGE_SIZE;
            mmu_map(top, p, p, MMU_BIT_READ | MMU_BIT_WRITE | MMU_BIT_GLOBAL);
        }
    }

    page_t* max_page = NULL;
    for (size_t i = 0; i < elf->header->program_header_num; i++) {
        elf_program_header_t* program_header = get_elf_program_header(elf, i);
        uint32_t flags_raw = program_header->flags;
        int flags = 0;
        if (flags_raw & 0x1)
            flags |= MMU_BIT_EXECUTE;
        else if (flags_raw & 0x2)
            flags |= MMU_BIT_WRITE;
        if (flags_raw & 0x4)
            flags |= MMU_BIT_READ;

        uint64_t page_count = (program_header->memory_size + PAGE_SIZE - 1) / PAGE_SIZE;
        for (uint64_t i = 0; i < page_count; i++) {
            void* virtual = (void*) program_header->virtual_addr + i * PAGE_SIZE;
            void* page = mmu_alloc(top, virtual, flags | MMU_BIT_USER);

            if ((page_t*) virtual > max_page)
                max_page = virtual;

            if (i * PAGE_SIZE < program_header->file_size) {
                memcpy(page, (void*) elf->header + program_header->offset + i * PAGE_SIZE, (program_header->file_size < (i + 1) * PAGE_SIZE ? program_header->file_size - i * PAGE_SIZE : PAGE_SIZE));
            }
        }
    }

    for (size_t i = 1; i <= stack_size; i++) {
        mmu_alloc(top, max_page + i, MMU_BIT_READ | MMU_BIT_WRITE | MMU_BIT_USER);
    }
    processes[pid].last_virtual_page = (void*) max_page + PAGE_SIZE * (stack_size + 1);
    processes[pid].xs[REGISTER_SP] = (uint64_t) processes[pid].last_virtual_page - 8;
    processes[pid].xs[REGISTER_FP] = processes[pid].xs[REGISTER_SP];
    processes[pid].last_virtual_page += PAGE_SIZE;

    if (args != NULL && arg_size != 0) {
        for (size_t i = 0; i < (arg_size + PAGE_SIZE - 1); i += PAGE_SIZE) {
            void* physical = mmu_alloc(top, processes[pid].last_virtual_page + i, MMU_BIT_READ | MMU_BIT_WRITE | MMU_BIT_USER);
            memcpy(physical, args + i, arg_size - i < PAGE_SIZE ? arg_size - i : PAGE_SIZE);
        }

        processes[pid].xs[REGISTER_A0] = (uint64_t) processes[pid].last_virtual_page;
        processes[pid].xs[REGISTER_A1] = arg_size;
        processes[pid].last_virtual_page += (arg_size + PAGE_SIZE - 1) / PAGE_SIZE * PAGE_SIZE + PAGE_SIZE;
    }

    process_t* parent = get_process(parent_pid);
    if (parent != NULL)
        processes[pid].user = parent->user;
    else
        processes[pid].user = 0;
    processes[pid].pid = pid;
    processes[pid].mmu_data = top;
    processes[pid].pc = elf->header->entry;
    processes[pid].state = PROCESS_STATE_WAIT;

    processes[pid].message_queue = alloc_pages(1);
    mmu_map(top, processes[pid].message_queue, processes[pid].message_queue, MMU_BIT_READ | MMU_BIT_WRITE);
    processes[pid].message_queue_start = 0;
    processes[pid].message_queue_end = 0;
    processes[pid].message_queue_len = 0;
    processes[pid].message_queue_cap = PAGE_SIZE / sizeof(process_message_t);

    if (top != get_mmu()) {
        mmu_level_1_t* current = get_mmu();
        remove_mmu_from_mmu(current, processes[pid].mmu_data);
        mmu_remove(current, processes[pid].message_queue);
    }
    return pid;
}

// switch_to_process(trap_t*, pid_t) -> void
// Jumps to the given process.
void switch_to_process(trap_t* trap, pid_t pid) {
    if (processes[trap->pid].state != PROCESS_STATE_DEAD && processes[trap->pid].state != PROCESS_STATE_WAIT) {
        if (processes[trap->pid].state == PROCESS_STATE_RUNNING)
            processes[trap->pid].state = PROCESS_STATE_WAIT;
        processes[trap->pid].pc = trap->pc;

        for (int i = 0; i < 32; i++) {
            processes[trap->pid].xs[i] = trap->xs[i];
        }

        for (int i = 0; i < 32; i++) {
            processes[trap->pid].fs[i] = trap->fs[i];
        }
    }

    trap->pid = pid;
    trap->pc = processes[pid].pc;

    for (int i = 0; i < 32; i++) {
        trap->xs[i] = processes[pid].xs[i];
    }

    for (int i = 0; i < 32; i++) {
        trap->fs[i] = processes[pid].fs[i];
    }

    set_mmu(processes[pid].mmu_data);

    processes[pid].state = PROCESS_STATE_RUNNING;
}

// get_next_waiting_process(pid_t) -> pid_t
// Searches for the next waiting process. Returns the given pid if not found.
pid_t get_next_waiting_process(pid_t pid) {
    while (true) {
        for (pid_t p = pid + 1; p != pid; p = (p + 1 < MAX_PID ? p + 1 : 0)) {
            if (processes[p].state == PROCESS_STATE_WAIT)
                return p;
            else if (processes[p].state == PROCESS_STATE_BLOCK_SLEEP) {
                time_t wait = processes[p].wake_on_time;
                time_t now = get_time();
                if ((wait.seconds == now.seconds && wait.micros <= now.micros) || (wait.seconds < now.seconds)) {
                    processes[p].xs[REGISTER_A0] = now.seconds;
                    processes[p].xs[REGISTER_A1] = now.micros;
                    return p;
                }
            } else if (processes[p].state == PROCESS_STATE_BLOCK_LOCK) {
                mmu_level_1_t* current = get_mmu();
                if (current != processes[p].mmu_data)
                    set_mmu(processes[p].mmu_data);
                if (lock_stop(processes[p].lock_ref, processes[p].lock_type, processes[p].lock_value)) {
                    processes[p].xs[REGISTER_A0] = 0;
                    return p;
                }
                if (current != processes[p].mmu_data)
                    set_mmu(current);
            }
        }

        if (processes[pid].state == PROCESS_STATE_RUNNING)
            return pid;
        else if (processes[pid].state == PROCESS_STATE_BLOCK_SLEEP) {
            time_t wait = processes[pid].wake_on_time;
            time_t now = get_time();
            if ((wait.seconds == now.seconds && wait.micros <= now.micros) || (wait.seconds < now.seconds)) {
                processes[pid].xs[REGISTER_A0] = now.seconds;
                processes[pid].xs[REGISTER_A1] = now.micros;
                return pid;
            }
        } else if (processes[pid].state == PROCESS_STATE_BLOCK_LOCK && lock_stop(processes[pid].lock_ref, processes[pid].lock_type, processes[pid].lock_value)) {
            processes[pid].xs[REGISTER_A0] = 0;
            return pid;
        }
    }
}

// get_process(pid_t) -> process_t*
// Gets the process associated with the pid.
process_t* get_process(pid_t pid) {
    if (processes[pid].state == PROCESS_STATE_DEAD)
        return NULL;
    return &processes[pid];
}

// kill_process(pid_t) -> void
// Kills a process.
void kill_process(pid_t pid) {
    if (processes[pid].state == PROCESS_STATE_DEAD)
        return;

    set_mmu(processes[0].mmu_data);
    clean_mmu_table(processes[pid].mmu_data);
    processes[pid].state = PROCESS_STATE_DEAD;
}

// enqueue_message_to_process(pid_t, process_message_t) -> bool
// Enqueues a message to a process's message queue. Returns true if successful and false if the process was not found or if the queue is full.
bool enqueue_message_to_process(pid_t recipient, process_message_t message) {
    process_t* process = get_process(recipient);
    if (process == NULL || process->message_queue_len >= process->message_queue_cap)
        return false;

    mmu_level_1_t* current = get_mmu();
    if (current != process->mmu_data)
        mmu_map(current, process->message_queue, process->message_queue, MMU_BIT_READ | MMU_BIT_WRITE);

    process->message_queue[process->message_queue_end] = message;
    process->message_queue_end++;
    if (process->message_queue_end >= process->message_queue_cap)
        process->message_queue_end = 0;
    process->message_queue_len++;
    if (current != process->mmu_data)
        mmu_remove(current, process->message_queue);
    return true;
}

// dequeue_message_from_process(pid_t, process_message_t) -> bool
// Dequeues a message from a process's message queue. Returns true if successful and false if the process was not found or if the queue is empty.
bool dequeue_message_from_process(pid_t pid, process_message_t* message) {
    process_t* process = get_process(pid);
    if (process == NULL || process->message_queue_len == 0)
        return false;

    *message = process->message_queue[process->message_queue_start];
    process->message_queue_start++;
    if (process->message_queue_start >= process->message_queue_cap)
        process->message_queue_start = 0;
    process->message_queue_len--;
    return true;
}
