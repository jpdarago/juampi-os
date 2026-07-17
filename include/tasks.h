#ifndef __TASK_H
#define __TASK_H

#include <types.h>
#include <klist.h>
#include <tss.h>
#include <elf.h>
#include <paging.h>
#include <vfs.h>
#include <proc.h>
#include <fs.h>

typedef enum status { P_RUNNING, P_BLOCKED, P_AVAILABLE, P_COMA } status;

typedef void (*signal_handler)(int);

#define SIGNALS 4

#define SIGINT 0
#define SIGKILL 1
#define SIGSTOP 2
#define SIGCONT 3

#define ERRINVPID -1
#define ERRINVSIG -2
#define ERRIGNSIG -3
#define ERROUTMEM -4
#define ERRGDTFULL -5
#define ERRINVFILE -6
#define ERRBIGEXEC -7
#define ERRNOTELF -8
#define ERRREAD -9
#define ERRIMPOSSIBLE -10
#define ERRARGTOOBIG -11
#define ERRTOOMANYARGS -12
#define ERRREADINGELF -13

#define EXEC_MAX_FSIZE (1024 * 1024)
#define EXEC_MAX_ARGC 16
#define EXEC_MAX_ARGSZ 128

typedef struct process_info {
    // Process identifier
    int pid;
    // Process state:
    // 	P_RUNNING: Currently has the CPU
    // 	P_BLOCKED: Blocked on I/O
    // 	P_AVAILABLE: Available and ready to run
    status status;
    // List of the pids of the child processes
    list_head children;
    // Structure with the parent process information
    struct process_info* parent;
    // TSS selector corresponding to this process
    short tss_selector;
    // Memory space where the tss of this process
    // and the explicit tss of the process are located
    void* tss_space_start;
    tss* tss;
    // How much time this process has remaining. It only makes
    // sense when status = P_RUNNING
    uint remaining_quantum;
    // Pointer to the page directory
    page_directory* page_dir;
    // Pointer to the child it is waiting for if it is
    // waiting for a child
    struct process_info* waiting_child;
    // Signal handlers
    signal_handler signal_handlers[SIGNALS];
    // Bitmap of signals pending to attend and to ignore (because
    // one of that type is already being processed)
    int signal_bitmap, ignore_bitmap;
    // Flag that tells whether the process is running
    // in kernel mode, so that it is not preempted if so
    bool kernel_mode;
    // Values where the esp and eip are when it returns from
    // the system call (only valid if it is in kernel mode)
    intptr prev_esp_pos, prev_eip_pos;
    // Position in the process list
    list_head process_list;
    // Position in the parent's process list
    list_head parent_list;
    // Position in the semaphore list where it sleeps
    list_head sem_queue;
    // List of file objects. Each number is an open file
    // descriptor
    file_object fds[MAX_FDS];
    // Current directory (current working directory)
    char cwd[FS_MAXLEN];
} __attribute__((__packed__)) process_info;

#define START_QUANTUM 50

extern list_head processes;

// Initializes the process scheduler
void scheduler_init(void);
// Helper for forking
int do_fork(intptr, gen_regs, uint, intptr);
// Replace the current process image with the one passed
// as parameter, with the indicated arguments on the stack.
// Returns 0 if it succeeded, -1 if it failed
int do_exec(char* filename, char** args, int_trace*, void*);
// Wait for the child process with the indicated pid
int do_wait(uint child_pid);
// Returns the tss of the next task to execute.
process_info* next_task(void);
// Loads the image from the elf file buffer passed as parameter
int elf_overlay_image(elf_file* e);
// Returns a pointer to the current task structure, NULL if
// there is no initial task
process_info* get_current_task(void);
// Switches task
void perform_task_switch(process_info*);
// Jumps to the initial task
void jump_to_initial(void*);
// Kills the current process
int do_exit(void);

// Puts the current process to sleep (thus releasing its quantum).
int do_sleep(void);

// Blocks the current process and releases its quantum
void block_current_process(void);
// Unblocks the current process
void wake_up(process_info* p);

void switch_kernel_mode(void);
void switch_user_mode(int_trace*);
bool kernel_mode(void);
bool is_preemptable(void);

// Signal handling
int do_kill(int pid, int signal);
int do_signal(int signal, signal_handler);

// These two functions are kludges to implement a syscall that
// deschedules a process (used to handle the SIGSTOP
// and SIGCONT signals).
int do_coma(void);
int do_clear_signal(int signal);

// Current working directory of the current process
void do_get_cwd(char* buf);
int do_set_cwd(const char*);

// My pid
int do_get_pid(void);

#endif
