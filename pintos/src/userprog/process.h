#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"
#include "threads/synch.h"

typedef int pid_t;

/* Structure representing an open file descriptor for a user process. */
struct fd_entry 
  {
    int fd;                     /* Integer descriptor visible to user program */
    struct file *file;          /* Underlying Pintos file pointer */
    struct list_elem elem;      /* Element in thread's open_files list */
  };

/* Shared control and synchronization block between parent and child processes. */
struct proc_sync 
  {
    tid_t tid;                  /* Process ID of the child */
    int exit_code;              /* Terminating exit status code */
    bool load_status;           /* True if binary loaded successfully */
    bool is_alive;              /* True while child process continues running */
    bool parent_alive;          /* True while parent process is still active */
    bool waited;                /* True if parent has already called wait */

    struct semaphore sema_load; /* Signaled after child attempts binary load */
    struct semaphore sema_wait; /* Signaled when child terminates */
    struct lock sync_lock;      /* Guards teardown and reference state */
    struct list_elem elem;      /* Element in parent's child_list */
  };

tid_t process_execute (const char *file_name);
int process_wait (tid_t);
void process_exit (void);
void process_activate (void);

#endif /* userprog/process.h */
