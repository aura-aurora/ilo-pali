#include <stdbool.h>

#include "console.h"
#include "hashmap.h"
#include "memory.h"
#include "process.h"
#include "queue.h"

hashmap_t* processes;
atomic_uint_fast64_t processes_lock = 0;

queue_t* jobs_queue;
atomic_bool mutating_jobs_queue = false;

hashmap_t* capabilities;
atomic_bool mutating_capabilities = false;

// init_processes() -> void
// Initialises process related stuff.
void init_processes() {
    processes = create_hashmap(sizeof(pid_t), sizeof(process_t));
    capabilities = create_hashmap(sizeof(capability_t), sizeof(process_channel_t));
    jobs_queue = create_queue(sizeof(pid_t));
}

static inline void LOCK_READ_PROCESSES() {
    while (true) {
        while (processes_lock & 1);

        uint64_t new = processes_lock + 2;
        uint64_t old = processes_lock;

        if (old & 1)
            continue;

        bool mutating = false;
        while (!atomic_compare_exchange_weak(&processes_lock, &old, new)) {
            if (old & 1) {
                mutating = true;
                break;
            }

            new = old + 2;
        }

        if (!mutating)
            return;
    }
}

static inline void LOCK_WRITE_PROCESSES() {
    while (true) {
        if (processes_lock)
            continue;

        uint64_t z = 0;
        if (atomic_compare_exchange_weak(&processes_lock, &z, 1))
            return;
    }
}

static inline void LOCK_RELEASE_PROCESSES() {
    // Only one mutable reference allowed, so we dont need an atomic thingy.
    if (processes_lock == 1)
        processes_lock = 0;
    else {
        uint64_t new = processes_lock - 2;
        uint64_t old = processes_lock;
        while (!atomic_compare_exchange_weak(&processes_lock, &old, new)) {
            new = old - 2;
        }
    }
}

// get_process(pid_t) -> process_t*
// Gets the process associated with the pid.
process_t* get_process(pid_t pid) {
    LOCK_READ_PROCESSES();
    process_t* process = hashmap_get(processes, &pid);
    if (process == NULL) {
        LOCK_RELEASE_PROCESSES();
        return NULL;
    }

    bool f = false;
    while (!atomic_compare_exchange_weak(&process->mutex_lock, &f, true)) {
        f = false;
    }

    return process;
}

// unlock_process(process_t*) -> void
// Unlocks the mutex associated with the process and frees a process hashmap reader.
void unlock_process(process_t* process) {
    if (process == NULL)
        return;
    process->mutex_lock = false;
    LOCK_RELEASE_PROCESSES();
}

// spawn_process_from_elf(char*, size_t, pid_t, elf_t*, size_t, void*, size_t) -> process_t*
// Spawns a process using the given elf file and parent pid. Returns NULL on failure.
process_t* spawn_process_from_elf(char* name, size_t name_size, pid_t parent_pid, elf_t* elf, size_t stack_size, void* args, size_t arg_size) {
    if (elf->header->type != ELF_EXECUTABLE) {
        console_puts("ELF file is not executable\n");
        return NULL;
    }

    LOCK_WRITE_PROCESSES();
    pid_t pid = 0;
    while (hashmap_get(processes, &pid) != NULL) {
        pid++;
        if (pid == (pid_t) -1)
            pid++;
    }

    mmu_level_1_t* top;
    if (pid == 0) {
        top = get_mmu();
    } else {
        top = create_mmu_table();
        identity_map_kernel(top, NULL, NULL, NULL);
    }

    page_t* max_page = NULL;
    for (size_t i = 0; i < elf->header->program_header_num; i++) {
        elf_program_header_t* program_header = get_elf_program_header(elf, i);

        if (program_header->type != 1)
            continue;

        uint32_t flags_raw = program_header->flags;
        int flags = 0;
        if (flags_raw & 0x1)
            flags |= MMU_BIT_EXECUTE;
        else if (flags_raw & 0x2)
            flags |= MMU_BIT_WRITE;
        if (flags_raw & 0x4)
            flags |= MMU_BIT_READ;

        size_t offset = program_header->virtual_addr % PAGE_SIZE;
        uint64_t page_count = (program_header->memory_size + offset + PAGE_SIZE - 1) / PAGE_SIZE;
        for (uint64_t i = 0; i < page_count; i++) {
            void* virtual = (void*) program_header->virtual_addr + i * PAGE_SIZE;
            void* page = mmu_alloc(top, virtual - offset, flags | MMU_BIT_USER);
            if (page == NULL) {
                intptr_t p = mmu_walk(top, virtual);
                if ((p & MMU_BIT_USER) == 0)
                    continue;
                page = MMU_UNWRAP(4, p);
            }

            page = kernel_space_phys2virtual(page);

            if ((page_t*) virtual > max_page)
                max_page = virtual;

            size_t size;
            if (program_header->file_size != 0) {
                if (program_header->file_size + offset < i * PAGE_SIZE) {
                    if ((i - 1) * PAGE_SIZE < program_header->file_size + offset)
                        size = program_header->file_size - (i - 1) * PAGE_SIZE;
                    else size = 0;
                } else size = PAGE_SIZE;
                void* source = (void*) elf->header + program_header->offset + i * PAGE_SIZE;
                if (i == 0) {
                    page += offset;
                    if (size > 0)
                        size -= offset;
                } else source -= offset;
                memcpy(page, source, size);
            }
        }
    }

    max_page = (void*) (((intptr_t) max_page + PAGE_SIZE - 1) / PAGE_SIZE * PAGE_SIZE);

    for (size_t i = 1; i <= stack_size; i++) {
        mmu_alloc(top, max_page + i, MMU_BIT_READ | MMU_BIT_WRITE | MMU_BIT_USER);
    }
    process_t process = { 0 };
    process.last_virtual_page = (void*) max_page + PAGE_SIZE * (stack_size + 1);
    process.xs[REGISTER_SP] = (uint64_t) process.last_virtual_page - 8;
    process.xs[REGISTER_FP] = process.xs[REGISTER_SP];
    process.last_virtual_page += PAGE_SIZE;

    if (args != NULL && arg_size != 0) {
        for (size_t i = 0; i < (arg_size + PAGE_SIZE - 1) / PAGE_SIZE * PAGE_SIZE; i += PAGE_SIZE) {
            void* physical = mmu_alloc(top, process.last_virtual_page + i, MMU_BIT_READ | MMU_BIT_WRITE | MMU_BIT_USER);
            memcpy(kernel_space_phys2virtual(physical), args + i, arg_size - i < PAGE_SIZE ? arg_size - i : PAGE_SIZE);
        }

        process.xs[REGISTER_A0] = (uint64_t) process.last_virtual_page;
        process.xs[REGISTER_A1] = arg_size;
        process.last_virtual_page += (arg_size + PAGE_SIZE - 1) / PAGE_SIZE * PAGE_SIZE + PAGE_SIZE;
    }

    process_t* parent = hashmap_get(processes, &parent_pid);
    if (parent != NULL) {
        bool f = false;
        while (!atomic_compare_exchange_weak(&parent->mutex_lock, &f, true)) {
            f = false;
        }

        process.user = parent->user;

        parent->mutex_lock = false;
    } else process.user = 0;

    process.name = malloc(name_size + 1);
    memcpy(process.name, name, name_size);
    process.name[name_size] = '\0';
    process.pid = pid;
    process.thread_source = -1;
    process.mmu_data = top;
    process.pc = elf->header->entry;
    process.state = PROCESS_STATE_WAIT;
    hashmap_insert(processes, &pid, &process);
    process_t* process_ptr = hashmap_get(processes, &pid);
    process_ptr->mutex_lock = true;
    processes_lock = processes_lock << 1;
    if (pid != 0) {
        bool f = false;
        while (!atomic_compare_exchange_weak(&mutating_jobs_queue, &f, true)) {
            f = false;
        }

        queue_enqueue(jobs_queue, &pid);
        mutating_jobs_queue = false;
    }
    return process_ptr;
}

// spawn_thread_from_func(pid_t, void*, size_t, void*, size_t) -> process_t*
// Spawns a thread from the given process. Returns NULL on failure.
process_t* spawn_thread_from_func(pid_t parent_pid, void* func, size_t stack_size, void* args, size_t arg_size) {
    LOCK_WRITE_PROCESSES();
    process_t* parent = hashmap_get(processes, &parent_pid);
    if (parent != NULL) {
        bool f = false;
        while (!atomic_compare_exchange_weak(&parent->mutex_lock, &f, true)) {
            f = false;
        }
    } else return NULL;
    if (parent == NULL)
        return NULL;
    if (parent->thread_source != (uint64_t) -1) {
        pid_t source = parent->thread_source;
        parent->mutex_lock = false;
        parent = hashmap_get(processes, &source);
        if (parent != NULL) {
            bool f = false;
            while (!atomic_compare_exchange_weak(&parent->mutex_lock, &f, true)) {
                f = false;
            }
        } else return NULL;
    }

    pid_t pid = 0;
    while (hashmap_get(processes, &pid) != NULL) {
        pid++;
        if (pid == (pid_t) -1)
            pid++;
    }

    process_t process;
    process.mmu_data = parent->mmu_data;
    process.thread_source = parent->pid;
    process.user = parent->user;

    for (size_t i = 1; i <= stack_size; i++) {
        mmu_alloc(parent->mmu_data, parent->last_virtual_page + PAGE_SIZE * i, MMU_BIT_READ | MMU_BIT_WRITE | MMU_BIT_USER);
    }
    parent->last_virtual_page += PAGE_SIZE * (stack_size + 1);
    process.xs[REGISTER_SP] = (uint64_t) parent->last_virtual_page - 8;
    process.xs[REGISTER_FP] = process.xs[REGISTER_SP];
    parent->last_virtual_page += PAGE_SIZE;

    process.xs[REGISTER_A0] = (uint64_t) args;
    process.xs[REGISTER_A1] = arg_size;

    size_t name_size = 0;
    for (; parent->name[name_size]; name_size++);
    process.name = malloc(name_size + 1);
    memcpy(process.name, parent->name, name_size);
    process.name[name_size] = '\0';
    process.pid = pid;
    process.pc = (uint64_t) func;
    process.state = PROCESS_STATE_WAIT;
    hashmap_insert(processes, &pid, &process);
    parent->mutex_lock = false;
    process_t* process_ptr = hashmap_get(processes, &pid);
    process_ptr->mutex_lock = true;
    processes_lock = processes_lock << 1;

    bool f = false;
    while (!atomic_compare_exchange_weak(&mutating_jobs_queue, &f, true)) {
        f = false;
    }

    queue_enqueue(jobs_queue, &pid);
    mutating_jobs_queue = false;

    return process_ptr;
}

// create_capability(capability_t*, pid_t, capability_t*, pid_t) -> void
// Creates a capability pair. The provided pointers are set to the capabilities.
void create_capability(capability_t* a, pid_t pa, capability_t* b, pid_t pb) {
    bool f = false;
    while (!atomic_compare_exchange_weak(&mutating_capabilities, &f, true)) {
        f = false;
    }

    *a = 0;
    *b = 0;
    do {
        // TODO: cryptographically secure random function
        *a += 1;
    } while (hashmap_get(capabilities, a) != NULL);

    hashmap_insert(capabilities, a, &(process_channel_t) {
        .message_queue = malloc(sizeof(process_message_t) * PROCESS_MESSAGE_QUEUE_SIZE),
        .start = 0,
        .end = 0,
        .len = 0,
        .receiver = pa,
    });

    do {
        // TODO: cryptographically secure random function
        *b += 1;
    } while (hashmap_get(capabilities, b) != NULL);

    hashmap_insert(capabilities, b, &(process_channel_t) {
        .message_queue = malloc(sizeof(process_message_t) * PROCESS_MESSAGE_QUEUE_SIZE),
        .start = 0,
        .end = 0,
        .len = 0,
        .sender = *a,
        .receiver = pb,
    });

    ((process_channel_t*) hashmap_get(capabilities, a))->sender = *b;
    mutating_capabilities = false;
}

// switch_to_process(trap_t*, pid_t) -> void
// Jumps to the given process.
void switch_to_process(trap_t* trap, pid_t pid) {
    process_t* last = trap->pid != pid ? get_process(trap->pid) : NULL;
    process_t* next = get_process(pid);

    if (last != NULL && last->state != PROCESS_STATE_DEAD) {
        bool f = false;
        while (!atomic_compare_exchange_weak(&mutating_jobs_queue, &f, true)) {
            f = false;
        }

        queue_enqueue(jobs_queue, &trap->pid);
        mutating_jobs_queue = false;

        if (last->state == PROCESS_STATE_RUNNING)
            last->state = PROCESS_STATE_WAIT;
        last->pc = trap->pc;

        for (int i = 0; i < 32; i++) {
            last->xs[i] = trap->xs[i];
        }

        for (int i = 0; i < 32; i++) {
            last->fs[i] = trap->fs[i];
        }
    }

    if (trap->pid != pid) {
        trap->pid = next->pid;
        trap->pc = next->pc;

        for (int i = 0; i < 32; i++) {
            trap->xs[i] = next->xs[i];
        }

        for (int i = 0; i < 32; i++) {
            trap->fs[i] = next->fs[i];
        }

        set_mmu(next->mmu_data);

        next->state = PROCESS_STATE_RUNNING;
    }

    if (last != next)
        unlock_process(last);
    unlock_process(next);
}

// get_next_waiting_process(pid_t) -> pid_t
// Searches for the next waiting process. Returns the given pid if not found.
pid_t get_next_waiting_process(pid_t pid) {
    // TODO: timer interrupt
    while (true) {
        bool f = false;
        while (!atomic_compare_exchange_weak(&mutating_jobs_queue, &f, true)) {
            f = false;
        }

        size_t len = queue_len(jobs_queue);
        while (len) {
            pid_t next_pid;
            queue_dequeue(jobs_queue, &next_pid);

            if (next_pid == pid) {
                len--;
                continue;
            }

            process_t* process = get_process(next_pid);
            if (process == NULL) {
                len--;
                continue;
            }
            if (process->state == PROCESS_STATE_WAIT) {
                mutating_jobs_queue = false;
                unlock_process(process);
                return next_pid;
            } else if (process->state == PROCESS_STATE_BLOCK_SLEEP) {
                time_t wait = process->wake_on_time;
                time_t now = get_time();

                if ((wait.seconds == now.seconds && wait.micros <= now.micros) || (wait.seconds < now.seconds)) {
                    process->xs[REGISTER_A0] = now.seconds;
                    process->xs[REGISTER_A1] = now.micros;
                    mutating_jobs_queue = false;
                    unlock_process(process);
                    return next_pid;
                }
            } else if (process->state == PROCESS_STATE_BLOCK_LOCK) {
                mmu_level_1_t* current = get_mmu();
                if (current != process->mmu_data)
                    set_mmu(process->mmu_data);
                if (lock_stop(process->lock_ref, process->lock_type, process->lock_value)) {
                    process->xs[REGISTER_A0] = 0;
                    mutating_jobs_queue = false;
                    unlock_process(process);
                    return next_pid;
                }
                if (current != process->mmu_data)
                    set_mmu(current);
            }

            queue_enqueue(jobs_queue, &next_pid);
            len--;
            unlock_process(process);
        }

        mutating_jobs_queue = false;
        process_t* current = get_process(pid);
        if (current->state == PROCESS_STATE_RUNNING) {
            unlock_process(current);
            return pid;
        }

        unlock_process(current);
    }
}

static bool find_associated_dead_capability(void* data, void* _key, void* value) {
    process_channel_t* channel = value;
    pid_t pid = (pid_t) data;
    return channel->receiver == pid;
}

// kill_process(pid_t) -> void
// Kills a process.
void kill_process(pid_t pid) {
    LOCK_WRITE_PROCESSES();
    process_t* process = hashmap_get(processes, &pid);
    if (process == NULL)
        return;

    bool f = false;
    while (!atomic_compare_exchange_weak(&process->mutex_lock, &f, true)) {
        f = false;
    }

    if (process->state == PROCESS_STATE_DEAD) {
        process->mutex_lock = false;
        return;
    }

    size_t i = 0, j = 0;
    f = false;
    while (!atomic_compare_exchange_weak(&mutating_capabilities, &f, true)) {
        f = false;
    }

    process_channel_t* channel;
    while ((channel = hashmap_find(capabilities, (void*) pid, find_associated_dead_capability, &i, &j))) {
        free(channel->message_queue);
        channel->message_queue = NULL;
        channel->start = 0;
        channel->end = 0;
        channel->len = 0;
        channel->receiver = 0;
    }
    mutating_capabilities = false;

    pid_t initd = 0;
    if (process->mmu_data == get_mmu())
        set_mmu(((process_t*) hashmap_get(processes, &initd))->mmu_data); // initd's mmu pointer never changes so this is okay

    if (process->thread_source == (pid_t) -1)
        clean_mmu_table(process->mmu_data);
    process->mutex_lock = false;
    hashmap_remove(processes, &pid);
    LOCK_RELEASE_PROCESSES();
}

// transfer_capability(capability_t, pid_t, pid_t) -> int
// Transfers the given capability to a new process. Returns 0 if successful, 1 if the capability is invalid, and 2 if the new owner doesn't exist.
int transfer_capability(capability_t capability, pid_t old_owner, pid_t new_owner) {
    bool f = false;
    while (!atomic_compare_exchange_weak(&mutating_capabilities, &f, true)) {
        f = false;
    }

    process_channel_t* channel = hashmap_get(capabilities, &capability);

    if (channel == NULL || channel->message_queue == NULL || channel->receiver != old_owner) {
        mutating_capabilities = false;
        return 1;
    }
    process_t* process = get_process(new_owner);
    if (process == NULL) {
        mutating_capabilities = false;
        return 2;
    }
    process_t* current = get_process(old_owner);
    if ((process->pid == 0 || new_owner == 0 || process->thread_source == 0) && current->thread_source != 0 && current->pid != 0) {
        mutating_capabilities = false;
        unlock_process(current);
        unlock_process(process);
        return 1;
    }
    channel->receiver = new_owner;
    mutating_capabilities = false;
    unlock_process(current);
    unlock_process(process);
    return 0;
}

// clone_capability(pid_t, capability_t, capability_t*) -> int
// Clones a capability. Returns 0 on success and 1 if the pid doesn't match or the capability is invalid.
int clone_capability(pid_t pid, capability_t original, capability_t* new) {
    bool f = false;
    while (!atomic_compare_exchange_weak(&mutating_capabilities, &f, true)) {
        f = false;
    }

    process_channel_t* channel = hashmap_get(capabilities, &original);
    if (channel == NULL) {
        mutating_capabilities = false;
        return 1;
    }

    process_t* receiver = get_process(channel->receiver);
    process_t* process = get_process(pid);
    if (receiver->pid != process->pid && receiver->pid != process->thread_source && receiver->thread_source != process->pid && receiver->thread_source != process->thread_source) {
        mutating_capabilities = false;
        unlock_process(receiver);
        unlock_process(process);
        return 1;
    }

    *new = 0;
    do {
        // TODO: cryptographically secure random function
        *new += 1;
    } while (hashmap_get(capabilities, new) != NULL);

    hashmap_insert(capabilities, new, &(process_channel_t) {
        .message_queue = malloc(sizeof(process_message_t) * PROCESS_MESSAGE_QUEUE_SIZE),
        .start = 0,
        .end = 0,
        .len = 0,
        .sender = original,
        .receiver = pid,
    });

    mutating_capabilities = false;
    unlock_process(receiver);
    unlock_process(process);
    return 0;
}

// enqueue_message_to_channel(capability_t, process_message_t) -> int
// Enqueues a message to a channel's message queue. Returns 0 if successful, 1 if the capability is invalid, 2 if the queue is full, and 3 if the connection has closed.
int enqueue_message_to_channel(capability_t capability, process_message_t message) {
    bool f = false;
    while (!atomic_compare_exchange_weak(&mutating_capabilities, &f, true)) {
        f = false;
    }

    process_channel_t* sender_channel = hashmap_get(capabilities, &capability);
    if (sender_channel == NULL) {
        mutating_capabilities = false;
        return 1;
    }
    process_channel_t* channel = hashmap_get(capabilities, &sender_channel->sender);
    if (channel == NULL) {
        mutating_capabilities = false;
        return 1;
    }

    if (channel->message_queue == NULL) {
        mutating_capabilities = false;
        return 3;
    }

    if (channel->len >= PROCESS_MESSAGE_QUEUE_SIZE) {
        mutating_capabilities = false;
        return 2;
    }

    channel->message_queue[channel->end++] = message;
    if (channel->end >= PROCESS_MESSAGE_QUEUE_SIZE)
        channel->end = 0;
    channel->len++;
    mutating_capabilities = false;
    return 0;
}

// enqueue_interrupt_to_channel(capability_t, uint32_t) -> int
// Enqueues an interrupt to a channel's message queue. Returns 0 if successful, 1 if the capability is invalid, 2 if the queue is full, and 3 if the connection has closed.
int enqueue_interrupt_to_channel(capability_t capability, uint32_t id) {
    bool f = false;
    while (!atomic_compare_exchange_weak(&mutating_capabilities, &f, true)) {
        f = false;
    }

    process_channel_t* channel = hashmap_get(capabilities, &capability);
    if (channel == NULL) {
        mutating_capabilities = false;
        return 1;
    }

    if (channel->message_queue == NULL) {
        mutating_capabilities = false;
        return 3;
    }

    if (channel->len >= PROCESS_MESSAGE_QUEUE_SIZE) {
        mutating_capabilities = false;
        return 2;
    }

    channel->message_queue[channel->end++] = (process_message_t) {
        .type = 4,
        .source = 0,
        .data = id,
        .metadata = 0,
    };
    if (channel->end >= PROCESS_MESSAGE_QUEUE_SIZE)
        channel->end = 0;
    channel->len++;
    mutating_capabilities = false;
    return 0;
}

// dequeue_message_from_channel(pid_t, capability_t, process_message_t*) -> int
// Dequeues a message from a channel's message queue. Returns 0 if successful, 1 if the capability is invalid, 2 if the queue is empty, and 3 if the channel has closed.
int dequeue_message_from_channel(pid_t pid, capability_t capability, process_message_t* message) {
    bool f = false;
    while (!atomic_compare_exchange_weak(&mutating_capabilities, &f, true)) {
        f = false;
    }

    process_channel_t* channel = hashmap_get(capabilities, &capability);
    if (channel == NULL || channel->receiver != pid) {
        mutating_capabilities = false;
        return 1;
    }

    if (channel->message_queue == NULL) {
        mutating_capabilities = false;
        return 3;
    }

    if (channel->len == 0) {
        mutating_capabilities = false;
        return 2;
    }

    *message = channel->message_queue[channel->start++];
    if (channel->start >= PROCESS_MESSAGE_QUEUE_SIZE)
        channel->start = 0;
    channel->len--;
    mutating_capabilities = false;
    return 0;
}

// capability_connects_to_initd(capability_t, pid_t) -> bool
// Returns true if the capability connects to initd or one of its threads.
bool capability_connects_to_initd(capability_t capability, pid_t pid) {
    bool f = false;
    while (!atomic_compare_exchange_weak(&mutating_capabilities, &f, true)) {
        f = false;
    }

    process_channel_t* channel = hashmap_get(capabilities, &capability);
    process_channel_t* sender = hashmap_get(capabilities, &channel->sender);
    if (channel == NULL || sender == NULL || channel->message_queue == NULL || sender->message_queue == NULL) {
        mutating_capabilities = false;
        return false;
    }
    if (channel->receiver == 0 || sender->receiver == 0) {
        mutating_capabilities = false;
        return true;
    }

    if (channel->receiver != pid) {
        process_t* receiver = get_process(channel->receiver);
        if (receiver == NULL) {
            mutating_capabilities = false;
            return false;
        }
        if (receiver->thread_source == 0) {
            mutating_capabilities = false;
            return true;
        }
        unlock_process(receiver);
    }

    if (sender->receiver != pid) {
        process_t* receiver = get_process(sender->receiver);
        if (receiver == NULL) {
            mutating_capabilities = false;
            return false;
        }
        bool result = receiver->thread_source == 0;
        unlock_process(receiver);
        mutating_capabilities = false;
        return result;
    }

    return false;
}
