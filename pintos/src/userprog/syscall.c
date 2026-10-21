#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "threads/synch.h"
#include "threads/malloc.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"
#include "devices/shutdown.h"
#include "devices/input.h"
#include "filesys/filesys.h"
#include "filesys/file.h"

/* Global lock to synchronize all file system operations */
static struct lock filesys_lock;

void
filesys_lock_acquire (void)
{
  lock_acquire (&filesys_lock);
}

void
filesys_lock_release (void)
{
  lock_release (&filesys_lock);
}

static void syscall_handler (struct intr_frame *);

/* Checks if a given user virtual address is mapped and within user space */
static bool
is_valid_user_ptr (const void *uaddr)
{
  if (uaddr == NULL || !is_user_vaddr (uaddr))
    return false;
  return pagedir_get_page (thread_current ()->pagedir, uaddr) != NULL;
}

/* Checks if an entire buffer of size bytes is within user memory and mapped */
static bool
is_valid_user_buffer (const void *buffer, size_t size)
{
  if (buffer == NULL)
    return false;
  if (size == 0)
    return is_valid_user_ptr (buffer);

  const uint8_t *start = (const uint8_t *) buffer;
  const uint8_t *end = start + size - 1;

  if (!is_user_vaddr (start) || !is_user_vaddr (end))
    return false;

  const uint8_t *p = pg_round_down (start);
  while (p <= end)
    {
      if (!is_valid_user_ptr (p))
        return false;
      p += PGSIZE;
    }
  return is_valid_user_ptr (end);
}

/* Validates that a string resides entirely in mapped user space up to '\0' */
static bool
is_valid_user_str (const char *ustr)
{
  if (ustr == NULL || !is_user_vaddr (ustr))
    return false;

  while (is_valid_user_ptr (ustr))
    {
      if (*ustr == '\0')
        return true;
      ustr++;
    }
  return false;
}

/* Terminates the user process with a given status, cleaning up held locks */
static void
sys_terminate (int status)
{
  if (lock_held_by_current_thread (&filesys_lock))
    filesys_lock_release ();

  struct thread *cur = thread_current ();
  if (cur->sync_node != NULL)
    cur->sync_node->exit_code = status;
  thread_exit ();
}

/* Finds a file descriptor entry by integer id in the current thread's list */
static struct fd_entry *
lookup_fd (int fd)
{
  struct thread *cur = thread_current ();
  struct list_elem *e;

  for (e = list_begin (&cur->open_files); e != list_end (&cur->open_files); e = list_next (e))
    {
      struct fd_entry *entry = list_entry (e, struct fd_entry, elem);
      if (entry->fd == fd)
        return entry;
    }
  return NULL;
}

void
syscall_init (void) 
{
  lock_init (&filesys_lock);
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
syscall_handler (struct intr_frame *f) 
{
  /* Validate that the stack pointer is within valid user memory */
  if (!is_valid_user_buffer (f->esp, sizeof (int)))
    sys_terminate (-1);

  int syscall_nr = *(int *) f->esp;

  switch (syscall_nr)
    {
    case SYS_HALT:
      {
        shutdown_power_off ();
        break;
      }

    case SYS_EXIT:
      {
        if (!is_valid_user_buffer ((int *) f->esp + 1, sizeof (int)))
          sys_terminate (-1);

        int status = *((int *) f->esp + 1);
        sys_terminate (status);
        break;
      }

    case SYS_EXEC:
      {
        if (!is_valid_user_buffer ((int *) f->esp + 1, sizeof (char *)))
          sys_terminate (-1);

        const char *cmd_line = (const char *) *((int *) f->esp + 1);
        if (!is_valid_user_str (cmd_line))
          sys_terminate (-1);

        f->eax = process_execute (cmd_line);
        break;
      }

    case SYS_WAIT:
      {
        if (!is_valid_user_buffer ((int *) f->esp + 1, sizeof (pid_t)))
          sys_terminate (-1);

        pid_t pid = (pid_t) *((int *) f->esp + 1);
        f->eax = process_wait (pid);
        break;
      }

    case SYS_CREATE:
      {
        if (!is_valid_user_buffer ((int *) f->esp + 1, sizeof (char *) + sizeof (unsigned)))
          sys_terminate (-1);

        const char *file = (const char *) *((int *) f->esp + 1);
        unsigned initial_size = (unsigned) *((int *) f->esp + 2);

        if (!is_valid_user_str (file))
          sys_terminate (-1);

        filesys_lock_acquire ();
        f->eax = filesys_create (file, initial_size);
        filesys_lock_release ();
        break;
      }

    case SYS_REMOVE:
      {
        if (!is_valid_user_buffer ((int *) f->esp + 1, sizeof (char *)))
          sys_terminate (-1);

        const char *file = (const char *) *((int *) f->esp + 1);
        if (!is_valid_user_str (file))
          sys_terminate (-1);

        filesys_lock_acquire ();
        f->eax = filesys_remove (file);
        filesys_lock_release ();
        break;
      }

    case SYS_OPEN:
      {
        if (!is_valid_user_buffer ((int *) f->esp + 1, sizeof (char *)))
          sys_terminate (-1);

        const char *file = (const char *) *((int *) f->esp + 1);
        if (!is_valid_user_str (file))
          sys_terminate (-1);

        filesys_lock_acquire ();
        struct file *opened_file = filesys_open (file);
        filesys_lock_release ();

        if (opened_file == NULL)
          {
            f->eax = -1;
            break;
          }

        struct fd_entry *entry = malloc (sizeof (struct fd_entry));
        if (entry == NULL)
          {
            filesys_lock_acquire ();
            file_close (opened_file);
            filesys_lock_release ();
            f->eax = -1;
            break;
          }

        struct thread *cur = thread_current ();
        entry->fd = cur->next_fd++;
        entry->file = opened_file;
        list_push_back (&cur->open_files, &entry->elem);
        f->eax = entry->fd;
        break;
      }

    case SYS_FILESIZE:
      {
        if (!is_valid_user_buffer ((int *) f->esp + 1, sizeof (int)))
          sys_terminate (-1);

        int fd = *((int *) f->esp + 1);
        struct fd_entry *entry = lookup_fd (fd);
        if (entry == NULL)
          {
            f->eax = -1;
            break;
          }

        filesys_lock_acquire ();
        f->eax = file_length (entry->file);
        filesys_lock_release ();
        break;
      }

    case SYS_READ:
      {
        if (!is_valid_user_buffer ((int *) f->esp + 1, 3 * sizeof (int)))
          sys_terminate (-1);

        int fd = *((int *) f->esp + 1);
        void *buffer = (void *) *((int *) f->esp + 2);
        unsigned size = (unsigned) *((int *) f->esp + 3);

        if (!is_valid_user_buffer (buffer, size))
          sys_terminate (-1);

        if (fd == 0)
          {
            uint8_t *buf = (uint8_t *) buffer;
            for (unsigned i = 0; i < size; i++)
              buf[i] = input_getc ();
            f->eax = size;
          }
        else if (fd == 1)
          {
            f->eax = -1;
          }
        else
          {
            struct fd_entry *entry = lookup_fd (fd);
            if (entry == NULL)
              {
                f->eax = -1;
                break;
              }

            filesys_lock_acquire ();
            f->eax = file_read (entry->file, buffer, size);
            filesys_lock_release ();
          }
        break;
      }

    case SYS_WRITE:
      {
        if (!is_valid_user_buffer ((int *) f->esp + 1, 3 * sizeof (int)))
          sys_terminate (-1);

        int fd = *((int *) f->esp + 1);
        const void *buffer = (const void *) *((int *) f->esp + 2);
        unsigned size = (unsigned) *((int *) f->esp + 3);

        if (!is_valid_user_buffer (buffer, size))
          sys_terminate (-1);

        if (fd == 1)
          {
            putbuf ((const char *) buffer, size);
            f->eax = size;
          }
        else if (fd == 0)
          {
            f->eax = -1;
          }
        else
          {
            struct fd_entry *entry = lookup_fd (fd);
            if (entry == NULL)
              {
                f->eax = -1;
                break;
              }

            filesys_lock_acquire ();
            f->eax = file_write (entry->file, buffer, size);
            filesys_lock_release ();
          }
        break;
      }

    case SYS_SEEK:
      {
        if (!is_valid_user_buffer ((int *) f->esp + 1, sizeof (int) + sizeof (unsigned)))
          sys_terminate (-1);

        int fd = *((int *) f->esp + 1);
        unsigned position = (unsigned) *((int *) f->esp + 2);

        struct fd_entry *entry = lookup_fd (fd);
        if (entry != NULL)
          {
            filesys_lock_acquire ();
            file_seek (entry->file, position);
            filesys_lock_release ();
          }
        break;
      }

    case SYS_TELL:
      {
        if (!is_valid_user_buffer ((int *) f->esp + 1, sizeof (int)))
          sys_terminate (-1);

        int fd = *((int *) f->esp + 1);
        struct fd_entry *entry = lookup_fd (fd);
        if (entry == NULL)
          {
            f->eax = -1;
            break;
          }

        filesys_lock_acquire ();
        f->eax = file_tell (entry->file);
        filesys_lock_release ();
        break;
      }

    case SYS_CLOSE:
      {
        if (!is_valid_user_buffer ((int *) f->esp + 1, sizeof (int)))
          sys_terminate (-1);

        int fd = *((int *) f->esp + 1);
        struct fd_entry *entry = lookup_fd (fd);
        if (entry != NULL)
          {
            list_remove (&entry->elem);
            filesys_lock_acquire ();
            file_close (entry->file);
            filesys_lock_release ();
            free (entry);
          }
        break;
      }

    default:
      sys_terminate (-1);
      break;
    }
}
