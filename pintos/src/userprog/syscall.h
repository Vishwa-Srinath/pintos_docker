#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

void syscall_init (void);

/* Global filesystem lock functions to ensure thread-safe filesystem access */
void filesys_lock_acquire (void);
void filesys_lock_release (void);

#endif /* userprog/syscall.h */
