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

/* lock for file system calls */
static struct lock fs_lock;

void
lock_fs (void)
{
  lock_acquire (&fs_lock);
}

void
unlock_fs (void)
{
  lock_release (&fs_lock);
}

static void syscall_handler (struct intr_frame *);

/* check user pointer */
static bool
check_user_ptr (const void *vaddr)
{
  if (vaddr == NULL || !is_user_vaddr (vaddr))
    return false;
  return pagedir_get_page (thread_current ()->pagedir, vaddr) != NULL;
}

/* check buffer */
static bool
check_user_buf (const void *buf, size_t sz)
{
  if (buf == NULL)
    return false;
  if (sz == 0)
    return check_user_ptr (buf);

  const uint8_t *s = (const uint8_t *) buf;
  const uint8_t *e = s + sz - 1;

  if (!is_user_vaddr (s) || !is_user_vaddr (e))
    return false;

  const uint8_t *curr = pg_round_down (s);
  while (curr <= e)
    {
      if (!check_user_ptr (curr))
        return false;
      curr += PGSIZE;
    }
  return check_user_ptr (e);
}

/* check string */
static bool
check_user_str (const char *s)
{
  if (s == NULL || !is_user_vaddr (s))
    return false;

  while (check_user_ptr (s))
    {
      if (*s == '\0')
        return true;
      s++;
    }
  return false;
}

/* exit process */
static void
exit_proc (int status)
{
  if (lock_held_by_current_thread (&fs_lock))
    lock_release (&fs_lock);

  struct thread *cur = thread_current ();
  if (cur->c_entry != NULL)
    cur->c_entry->exit_status = status;
  thread_exit ();
}

/* find fd */
static struct f_desc *
find_fd (int fd)
{
  struct thread *cur = thread_current ();
  struct list_elem *e;
  for (e = list_begin (&cur->fds); e != list_end (&cur->fds); e = list_next (e))
    {
      struct f_desc *desc = list_entry (e, struct f_desc, elem);
      if (desc->fd == fd)
        return desc;
    }
  return NULL;
}

void
syscall_init (void) 
{
  lock_init (&fs_lock);
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
syscall_handler (struct intr_frame *f) 
{
  /* check esp */
  if (!check_user_buf (f->esp, sizeof (int)))
    exit_proc (-1);

  int *p = (int *) f->esp;
  int call_num = p[0];

  switch (call_num)
    {
    case SYS_HALT:
      shutdown_power_off ();
      break;

    case SYS_EXIT:
      if (!check_user_buf (&p[1], sizeof (int)))
        exit_proc (-1);
      exit_proc (p[1]);
      break;

    case SYS_EXEC:
      if (!check_user_buf (&p[1], sizeof (char *)))
        exit_proc (-1);
      if (!check_user_str ((const char *) p[1]))
        exit_proc (-1);
      f->eax = process_execute ((const char *) p[1]);
      break;

    case SYS_WAIT:
      if (!check_user_buf (&p[1], sizeof (pid_t)))
        exit_proc (-1);
      f->eax = process_wait ((pid_t) p[1]);
      break;

    case SYS_CREATE:
      if (!check_user_buf (&p[1], sizeof (char *) + sizeof (unsigned)))
        exit_proc (-1);
      if (!check_user_str ((const char *) p[1]))
        exit_proc (-1);
      lock_fs ();
      f->eax = filesys_create ((const char *) p[1], (unsigned) p[2]);
      unlock_fs ();
      break;

    case SYS_REMOVE:
      if (!check_user_buf (&p[1], sizeof (char *)))
        exit_proc (-1);
      if (!check_user_str ((const char *) p[1]))
        exit_proc (-1);
      lock_fs ();
      f->eax = filesys_remove ((const char *) p[1]);
      unlock_fs ();
      break;

    case SYS_OPEN:
      {
        if (!check_user_buf (&p[1], sizeof (char *)))
          exit_proc (-1);
        if (!check_user_str ((const char *) p[1]))
          exit_proc (-1);

        lock_fs ();
        struct file *f_ptr = filesys_open ((const char *) p[1]);
        unlock_fs ();

        if (f_ptr == NULL)
          {
            f->eax = -1;
            break;
          }

        struct f_desc *desc = malloc (sizeof (struct f_desc));
        if (desc == NULL)
          {
            lock_fs ();
            file_close (f_ptr);
            unlock_fs ();
            f->eax = -1;
            break;
          }

        struct thread *cur = thread_current ();
        desc->fd = cur->cur_fd++;
        desc->fp = f_ptr;
        list_push_back (&cur->fds, &desc->elem);
        f->eax = desc->fd;
        break;
      }

    case SYS_FILESIZE:
      {
        if (!check_user_buf (&p[1], sizeof (int)))
          exit_proc (-1);

        struct f_desc *desc = find_fd (p[1]);
        if (desc == NULL)
          {
            f->eax = -1;
            break;
          }

        lock_fs ();
        f->eax = file_length (desc->fp);
        unlock_fs ();
        break;
      }

    case SYS_READ:
      {
        if (!check_user_buf (&p[1], 3 * sizeof (int)))
          exit_proc (-1);

        int fd = p[1];
        void *buf = (void *) p[2];
        unsigned len = (unsigned) p[3];

        if (!check_user_buf (buf, len))
          exit_proc (-1);

        if (fd == 0)
          {
            /* stdin */
            uint8_t *b = (uint8_t *) buf;
            for (unsigned i = 0; i < len; i++)
              b[i] = input_getc ();
            f->eax = len;
          }
        else if (fd == 1)
          {
            f->eax = -1;
          }
        else
          {
            struct f_desc *desc = find_fd (fd);
            if (desc == NULL)
              {
                f->eax = -1;
                break;
              }

            lock_fs ();
            f->eax = file_read (desc->fp, buf, len);
            unlock_fs ();
          }
        break;
      }

    case SYS_WRITE:
      {
        if (!check_user_buf (&p[1], 3 * sizeof (int)))
          exit_proc (-1);

        int fd = p[1];
        const void *buf = (const void *) p[2];
        unsigned len = (unsigned) p[3];

        if (!check_user_buf (buf, len))
          exit_proc (-1);

        if (fd == 1)
          {
            /* stdout */
            putbuf ((const char *) buf, len);
            f->eax = len;
          }
        else if (fd == 0)
          {
            f->eax = -1;
          }
        else
          {
            struct f_desc *desc = find_fd (fd);
            if (desc == NULL)
              {
                f->eax = -1;
                break;
              }

            lock_fs ();
            f->eax = file_write (desc->fp, buf, len);
            unlock_fs ();
          }
        break;
      }

    case SYS_SEEK:
      {
        if (!check_user_buf (&p[1], sizeof (int) + sizeof (unsigned)))
          exit_proc (-1);

        struct f_desc *desc = find_fd (p[1]);
        if (desc != NULL)
          {
            lock_fs ();
            file_seek (desc->fp, (unsigned) p[2]);
            unlock_fs ();
          }
        break;
      }

    case SYS_TELL:
      {
        if (!check_user_buf (&p[1], sizeof (int)))
          exit_proc (-1);

        struct f_desc *desc = find_fd (p[1]);
        if (desc == NULL)
          {
            f->eax = -1;
            break;
          }

        lock_fs ();
        f->eax = file_tell (desc->fp);
        unlock_fs ();
        break;
      }

    case SYS_CLOSE:
      {
        if (!check_user_buf (&p[1], sizeof (int)))
          exit_proc (-1);

        struct f_desc *desc = find_fd (p[1]);
        if (desc != NULL)
          {
            list_remove (&desc->elem);
            lock_fs ();
            file_close (desc->fp);
            unlock_fs ();
            free (desc);
          }
        break;
      }

    default:
      exit_proc (-1);
      break;
    }
}
