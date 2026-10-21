#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "threads/malloc.h"
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "userprog/syscall.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"

/* helper struct to pass data to new thread */
struct proc_aux 
  {
    char *fn_copy;
    struct child_entry *c_entry;
  };

static thread_func start_process NO_RETURN;
static bool load (const char *cmdline, void (**eip) (void), void **esp);
static bool setup_args (const char *cmd_line, void **esp);

/* Starts a new thread running a user program loaded from FILENAME. */
tid_t
process_execute (const char *file_name) 
{
  char *fn_copy;
  char *tmp_name;
  char *prog;
  char *save_ptr;
  tid_t tid;

  /* copy file_name */
  fn_copy = palloc_get_page (0);
  if (fn_copy == NULL)
    return TID_ERROR;
  strlcpy (fn_copy, file_name, PGSIZE);

  /* extract prog name for thread name */
  tmp_name = palloc_get_page (0);
  if (tmp_name == NULL)
    {
      palloc_free_page (fn_copy);
      return TID_ERROR;
    }
  strlcpy (tmp_name, file_name, PGSIZE);
  prog = strtok_r (tmp_name, " ", &save_ptr);
  if (prog == NULL)
    {
      palloc_free_page (tmp_name);
      palloc_free_page (fn_copy);
      return TID_ERROR;
    }

  /* allocate child_entry */
  struct child_entry *child = malloc (sizeof (struct child_entry));
  if (child == NULL)
    {
      palloc_free_page (tmp_name);
      palloc_free_page (fn_copy);
      return TID_ERROR;
    }

  child->exit_status = -1;
  child->load_ok = false;
  child->is_alive = true;
  child->parent_alive = true;
  child->waited = false;
  sema_init (&child->s_load, 0);
  sema_init (&child->s_exit, 0);
  lock_init (&child->lk);
  list_push_back (&thread_current ()->children, &child->elem);

  struct proc_aux *aux = malloc (sizeof (struct proc_aux));
  if (aux == NULL)
    {
      list_remove (&child->elem);
      free (child);
      palloc_free_page (tmp_name);
      palloc_free_page (fn_copy);
      return TID_ERROR;
    }
  aux->fn_copy = fn_copy;
  aux->c_entry = child;

  /* create thread */
  tid = thread_create (prog, PRI_DEFAULT, start_process, aux);
  palloc_free_page (tmp_name);

  if (tid == TID_ERROR)
    {
      list_remove (&child->elem);
      free (child);
      free (aux);
      palloc_free_page (fn_copy);
      return TID_ERROR;
    }

  child->tid = tid;

  /* wait for child load */
  sema_down (&child->s_load);

  if (!child->load_ok)
    {
      list_remove (&child->elem);
      free (child);
      return TID_ERROR;
    }

  return tid;
}

/* A thread function that loads a user process and starts it running. */
static void
start_process (void *aux_)
{
  struct proc_aux *aux = (struct proc_aux *) aux_;
  char *file_name = aux->fn_copy;
  struct child_entry *child = aux->c_entry;
  free (aux);

  struct thread *cur = thread_current ();
  cur->c_entry = child;

  struct intr_frame if_;
  bool success;

  memset (&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;
  success = load (file_name, &if_.eip, &if_.esp);

  if (!success)
    {
      child->load_ok = false;
      cur->c_entry = NULL;
      sema_up (&child->s_load);
      palloc_free_page (file_name);
      thread_exit ();
    }

  child->load_ok = true;
  sema_up (&child->s_load);
  palloc_free_page (file_name);

  asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory");
  NOT_REACHED ();
}

/* Waits for thread TID to die and returns its exit status. */
int
process_wait (tid_t child_tid) 
{
  struct thread *cur = thread_current ();
  struct list_elem *e;

  for (e = list_begin (&cur->children); e != list_end (&cur->children); e = list_next (e))
    {
      struct child_entry *child = list_entry (e, struct child_entry, elem);
      if (child->tid == child_tid)
        {
          if (child->waited)
            return -1;
          child->waited = true;

          sema_down (&child->s_exit);
          int st = child->exit_status;

          list_remove (&child->elem);
          free (child);

          return st;
        }
    }

  return -1;
}

/* Free the current process's resources. */
void
process_exit (void)
{
  struct thread *cur = thread_current ();
  uint32_t *pd;

  /* print exit message if user process */
  if (cur->c_entry != NULL)
    printf ("%s: exit(%d)\n", cur->name, cur->c_entry->exit_status);

  /* close all open files */
  lock_fs ();
  while (!list_empty (&cur->fds))
    {
      struct list_elem *e = list_pop_front (&cur->fds);
      struct f_desc *desc = list_entry (e, struct f_desc, elem);
      file_close (desc->fp);
      free (desc);
    }

  /* close running executable */
  if (cur->my_file != NULL)
    {
      file_allow_write (cur->my_file);
      file_close (cur->my_file);
      cur->my_file = NULL;
    }
  unlock_fs ();

  /* update my entry in parent's list */
  if (cur->c_entry != NULL)
    {
      struct child_entry *child = cur->c_entry;
      lock_acquire (&child->lk);
      child->is_alive = false;
      sema_up (&child->s_exit);
      bool orphan = !child->parent_alive;
      lock_release (&child->lk);

      if (orphan)
        free (child);
    }

  /* update my children */
  while (!list_empty (&cur->children))
    {
      struct list_elem *e = list_pop_front (&cur->children);
      struct child_entry *child = list_entry (e, struct child_entry, elem);

      lock_acquire (&child->lk);
      child->parent_alive = false;
      bool dead = !child->is_alive;
      lock_release (&child->lk);

      if (dead)
        free (child);
    }

  /* destroy page directory */
  pd = cur->pagedir;
  if (pd != NULL) 
    {
      cur->pagedir = NULL;
      pagedir_activate (NULL);
      pagedir_destroy (pd);
    }
}

/* Sets up the CPU for running user code in the current thread. */
void
process_activate (void)
{
  struct thread *t = thread_current ();
  pagedir_activate (t->pagedir);
  tss_update ();
}

/* ELF types. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

#define PE32Wx PRIx32
#define PE32Ax PRIx32
#define PE32Ox PRIx32
#define PE32Hx PRIx16

/* Executable header. */
struct Elf32_Ehdr
  {
    unsigned char e_ident[16];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
  };

/* Program header. */
struct Elf32_Phdr
  {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
  };

#define PT_NULL    0
#define PT_LOAD    1
#define PT_DYNAMIC 2
#define PT_INTERP  3
#define PT_NOTE    4
#define PT_SHLIB   5
#define PT_PHDR    6
#define PT_STACK   0x6474e551

#define PF_X 1
#define PF_W 2
#define PF_R 4

static bool setup_stack (void **esp);
static bool validate_segment (const struct Elf32_Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable);

/* parses command line and sets up user stack */
static bool
setup_args (const char *cmd_line, void **esp)
{
  char *cmd_buf = palloc_get_page (0);
  if (cmd_buf == NULL)
    return false;
  strlcpy (cmd_buf, cmd_line, PGSIZE);

  char *argv[128];
  int argc = 0;
  char *tok, *save_ptr;

  for (tok = strtok_r (cmd_buf, " ", &save_ptr); tok != NULL;
       tok = strtok_r (NULL, " ", &save_ptr))
    {
      argv[argc++] = tok;
      if (argc >= 128)
        break;
    }

  uintptr_t arg_addrs[128];

  /* 1. push actual string chars in reverse order */
  for (int i = argc - 1; i >= 0; i--)
    {
      size_t len = strlen (argv[i]) + 1;
      *esp = (void *) ((uint8_t *) *esp - len);
      memcpy (*esp, argv[i], len);
      arg_addrs[i] = (uintptr_t) *esp;
    }

  /* 2. word align stack pointer (4 bytes) */
  uintptr_t pad = ((uintptr_t) *esp) % 4;
  if (pad != 0)
    {
      *esp = (void *) ((uint8_t *) *esp - pad);
      memset (*esp, 0, pad);
    }

  /* 3. push null sentinel for argv[argc] */
  *esp = (void *) ((uint8_t *) *esp - sizeof (char *));
  *(char **) *esp = NULL;

  /* 4. push string addresses */
  for (int i = argc - 1; i >= 0; i--)
    {
      *esp = (void *) ((uint8_t *) *esp - sizeof (char *));
      *(char **) *esp = (char *) arg_addrs[i];
    }

  /* 5. push argv pointer */
  char **argv_head = (char **) *esp;
  *esp = (void *) ((uint8_t *) *esp - sizeof (char **));
  *(char ***) *esp = argv_head;

  /* 6. push argc */
  *esp = (void *) ((uint8_t *) *esp - sizeof (int));
  *(int *) *esp = argc;

  /* 7. push fake return address */
  *esp = (void *) ((uint8_t *) *esp - sizeof (void *));
  *(void **) *esp = NULL;

  palloc_free_page (cmd_buf);

  /* stack cannot exceed 1 page */
  return ((uint8_t *) PHYS_BASE - (uint8_t *) *esp) <= PGSIZE;
}

/* Loads an ELF executable from FILE_NAME into current thread. */
bool
load (const char *file_name, void (**eip) (void), void **esp) 
{
  struct thread *t = thread_current ();
  struct Elf32_Ehdr ehdr;
  struct file *file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;

  /* extract binary name */
  char bin_name[128];
  char *save_ptr;
  char *temp_copy = palloc_get_page (0);
  if (temp_copy == NULL)
    return false;
  strlcpy (temp_copy, file_name, PGSIZE);
  char *tok = strtok_r (temp_copy, " ", &save_ptr);
  if (tok != NULL)
    strlcpy (bin_name, tok, sizeof bin_name);
  else
    bin_name[0] = '\0';
  palloc_free_page (temp_copy);

  /* pagedir init */
  t->pagedir = pagedir_create ();
  if (t->pagedir == NULL) 
    return false;
  process_activate ();

  /* open executable with lock */
  lock_fs ();
  file = filesys_open (bin_name);
  if (file == NULL) 
    {
      unlock_fs ();
      printf ("load: %s: open failed\n", bin_name);
      return false; 
    }

  /* Read and verify executable header. */
  if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
      || memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7)
      || ehdr.e_type != 2
      || ehdr.e_machine != 3
      || ehdr.e_version != 1
      || ehdr.e_phentsize != sizeof (struct Elf32_Phdr)
      || ehdr.e_phnum > 1024) 
    {
      printf ("load: %s: error loading executable\n", bin_name);
      goto done; 
    }

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++) 
    {
      struct Elf32_Phdr phdr;

      if (file_ofs < 0 || file_ofs > file_length (file))
        goto done;
      file_seek (file, file_ofs);

      if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
        goto done;
      file_ofs += sizeof phdr;
      switch (phdr.p_type) 
        {
        case PT_NULL:
        case PT_NOTE:
        case PT_PHDR:
        case PT_STACK:
        default:
          break;
        case PT_DYNAMIC:
        case PT_INTERP:
        case PT_SHLIB:
          goto done;
        case PT_LOAD:
          if (validate_segment (&phdr, file)) 
            {
              bool writable = (phdr.p_flags & PF_W) != 0;
              uint32_t file_page = phdr.p_offset & ~PGMASK;
              uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
              uint32_t page_offset = phdr.p_vaddr & PGMASK;
              uint32_t read_bytes, zero_bytes;
              if (phdr.p_filesz > 0)
                {
                  read_bytes = page_offset + phdr.p_filesz;
                  zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
                                - read_bytes);
                }
              else 
                {
                  read_bytes = 0;
                  zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
                }
              if (!load_segment (file, file_page, (void *) mem_page,
                                 read_bytes, zero_bytes, writable))
                goto done;
            }
          else
            goto done;
          break;
        }
    }

  /* Set up stack and arguments */
  if (!setup_stack (esp))
    goto done;

  if (!setup_args (file_name, esp))
    goto done;

  *eip = (void (*) (void)) ehdr.e_entry;

  /* deny write to running binary */
  file_deny_write (file);
  t->my_file = file;

  success = true;

 done:
  if (!success && file != NULL)
    file_close (file);
  unlock_fs ();
  return success;
}

static bool install_page (void *upage, void *kpage, bool writable);

static bool
validate_segment (const struct Elf32_Phdr *phdr, struct file *file) 
{
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK)) 
    return false; 
  if (phdr->p_offset > (Elf32_Off) file_length (file)) 
    return false;
  if (phdr->p_memsz < phdr->p_filesz) 
    return false; 
  if (phdr->p_memsz == 0)
    return false;
  if (!is_user_vaddr ((void *) phdr->p_vaddr))
    return false;
  if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
    return false;

  if (phdr->p_vaddr < (Elf32_Addr) PHYS_BASE)
    {
      if (phdr->p_offset + phdr->p_filesz > (Elf32_Off) file_length (file))
        return false;
    }

  return true;
}

static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable) 
{
  ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT (pg_ofs (upage) == 0);
  ASSERT (ofs % PGSIZE == 0);

  file_seek (file, ofs);
  while (read_bytes > 0 || zero_bytes > 0) 
    {
      size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
      size_t page_zero_bytes = PGSIZE - page_read_bytes;

      uint8_t *kpage = palloc_get_page (PAL_USER);
      if (kpage == NULL)
        return false;

      if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes)
        {
          palloc_free_page (kpage);
          return false; 
        }
      memset (kpage + page_read_bytes, 0, page_zero_bytes);

      if (!install_page (upage, kpage, writable)) 
        {
          palloc_free_page (kpage);
          return false; 
        }

      read_bytes -= page_read_bytes;
      zero_bytes -= page_zero_bytes;
      upage += PGSIZE;
    }
  return true;
}

static bool
setup_stack (void **esp) 
{
  uint8_t *kpage;
  bool success = false;

  kpage = palloc_get_page (PAL_USER | PAL_ZERO);
  if (kpage != NULL) 
    {
      success = install_page (((uint8_t *) PHYS_BASE) - PGSIZE, kpage, true);
      if (success)
        *esp = PHYS_BASE;
      else
        palloc_free_page (kpage);
    }
  return success;
}

static bool
install_page (void *upage, void *kpage, bool writable)
{
  struct thread *t = thread_current ();
  return (pagedir_get_page (t->pagedir, upage) == NULL
          && pagedir_set_page (t->pagedir, upage, kpage, writable));
}
