#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

void syscall_init (void);

/* lock for file system calls */
void lock_fs (void);
void unlock_fs (void);

#endif /* userprog/syscall.h */
