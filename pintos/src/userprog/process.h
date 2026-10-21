#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"
#include "threads/synch.h"

typedef int pid_t;

/* file descriptor entry */
struct f_desc 
  {
    int fd;
    struct file *fp;
    struct list_elem elem;
  };

/* tracking child process info and exit status */
struct child_entry 
  {
    tid_t tid;
    int exit_status;
    bool load_ok;
    bool is_alive;
    bool parent_alive;
    bool waited;

    struct semaphore s_load;
    struct semaphore s_exit;
    struct lock lk;
    struct list_elem elem;
  };

tid_t process_execute (const char *file_name);
int process_wait (tid_t);
void process_exit (void);
void process_activate (void);

#endif /* userprog/process.h */
