# things to do
- [ ] review internals
  - [X] check memory allocation/management
  - [ ] allow for sharing pages
  - [ ] walk mmu to check pointer usage before using userspace memory
  - [ ] check interrupts (use interrupt vector as a jump table)
  - [ ] handle page faults appropriately
  - [ ] check elf file parsing and loading
  - [ ] fix processes (WIP)
- [ ] add better kernelspace locking mechanisms (and use them!)
- [ ] change how processes work
  - [ ] make scheduling algorithm smarter
  - [ ] have different queues for different process states/priorities
  - [X] process control block
  - [X] make the collection of processes an array instead of a hashmap (why is it a hashmap???)
  - [ ] add user id (uid)
  - [X] add parent pid (ppid)
  - [X] add group id (gid)
  - [X] add thread id (tid)
  - [ ] when a process dies, its exit status gets queued to a parent wait queue
    - [ ] no zombie process created (can reuse pids)
    - [ ] if the parent dies, the exit status queue disappears
  - [ ] use the entire bottom half of virtual memory lazily
- [ ] change how files work
  - [ ] unixy "everything is a file"
  - [ ] file system a la linux (inodes and mounts)
  - [ ] types of files:
    - [ ] regular
    - [ ] directory
    - [ ] symbolic link
    - [ ] char device
    - [ ] block device
    - [ ] fifo
  - [ ] unix style kernel file organisation
    - [ ] process control block has pointer to file descriptor table
    - [ ] file descriptor table points to open file entry
  - [ ] file buffering!
  - [ ] for device files, kernel keeps track of:
    - [ ] which driver controls it
    - [ ] what device id it has for the driver
    - [ ] any other relevant metadata
  - [ ] for opened files, kernel keeps track of:
    - [ ] what driver controls it
    - [ ] what opened file id it has
    - [ ] any other relevant metadata
  - [ ] kernel knows nothing else about files
- [ ] change how ipc works
  - [ ] entirely managed by kernel
  - [ ] fifo
    - [ ] packet mode - send packets of a fixed size
    - [ ] stream mode - send continuous data
    - [ ] buffer in kernelspace
    - [ ] no userspace driver despite being a file
  - [ ] shared memory
  - [ ] signals
    - [ ] not like unix signals cuz those are hell
    - [ ] process can request to send signal
      - [ ] allowed if same process group or root uid
    - [ ] kernel can send signals
    - [ ] process can wait for signal
    - [ ] signals are sent to everything with that thread id
    - [ ] signals that are sent to specific threads:
      - [ ] interrupt
      - [ ] trap signals
      - [ ] if explicitly told to send to a single thread
    - [ ] list of signals
      - [ ] interrupt - device interrupt
      - [ ] kill - kills process unconditionally
      - [ ] quit - request to kill a process
        - [ ] can be caught unix style (but terminates after)
      - [ ] stop - suspends the process unconditionally
      - [ ] suspend - suspends the process
        - [ ] can be caught unix style (but suspends after)
      - [ ] continue - resumes a process after being suspended
      - [ ] mem trap - a cpu trap was triggered from memory access
        - [ ] can only terminate or be handled unix style
      - [ ] floating point trap - a cpu trap was triggered from a floating point error
        - [ ] can only terminate or be handled unix style
      - [ ] arithmetic trap - a cpu trap was triggered from integer error
        - [ ] can only terminate or be handled unix style
    - [ ] signals that cant be ignored: kill and stop
- [ ] change how drivers work
  - [ ] processes with uid 0 and driver permission
    - [ ] only the init process can give driver permission
  - [ ] a process can request a driver page
    - [ ] gets driver page iff its a driver process
    - [ ] driver page is read only
    - [ ] requesting driver info puts it into the driver page
  - [ ] figure out how to request/create specific devices and handle their interrupts (jumptable?)
  - [ ] ioctl to send commands to driver
  - [ ] driver is a userspace process
  - [ ] increase priority of driver if a process has to sleep to get device data/info
  - [ ] typical organisation of driver
    - [ ] main thread
      - [ ] first driver thread spawned
      - [ ] only one main thread
      - [ ] waits for messages from kernel for spawning task threads
      - [ ] delegates tasks to a task thread
      - [ ] delegates device interrupts to task thread
      - [ ] maintains shared structures
    - [ ] task thread
      - [ ] one per file descriptor
      - [ ] waits for task from process with file descriptor ownership
      - [ ] performs task
      - [ ] may perform io
      - [ ] sends result of task directly to requester
- [ ] add swapping processes in and out of disk
- [ ] add support for relocatable elf files (and shared libraries!)
- [ ] make a real libc