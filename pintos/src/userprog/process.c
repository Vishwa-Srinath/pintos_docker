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

/* Structure to transport initial process parameters to start_process thread */
struct spawn_payload 
  {
    char *fn_copy;
    struct proc_sync *sync;
  };

static thread_func start_process NO_RETURN;
static bool load (const char *cmdline, void (**eip) (void), void **esp);
static bool push_command_args (const char *cmd_line, void **esp);

/* Starts a new thread running a user program loaded from
   FILENAME. Returns the new process's thread id, or TID_ERROR
   if the thread cannot be created or fails to load. */
tid_t
process_execute (const char *file_name) 
{
  char *fn_copy;
  char *name_copy;
  char *prog_name;
  char *save_ptr;
  tid_t tid;

  /* Make a copy of FILE_NAME.
     Otherwise there's a race between the caller and load(). */
  fn_copy = palloc_get_page (0);
  if (fn_copy == NULL)
    return TID_ERROR;
  strlcpy (fn_copy, file_name, PGSIZE);

  /* Extract the base program name to identify the thread */
  name_copy = palloc_get_page (0);
  if (name_copy == NULL)
    {
      palloc_free_page (fn_copy);
      return TID_ERROR;
    }
  strlcpy (name_copy, file_name, PGSIZE);
  prog_name = strtok_r (name_copy, " ", &save_ptr);
  if (prog_name == NULL)
    {
      palloc_free_page (name_copy);
      palloc_free_page (fn_copy);
      return TID_ERROR;
    }

  /* Allocate synchronization node for the parent-child relationship */
  struct proc_sync *child_sync = malloc (sizeof (struct proc_sync));
  if (child_sync == NULL)
    {
      palloc_free_page (name_copy);
      palloc_free_page (fn_copy);
      return TID_ERROR;
    }

  child_sync->exit_code = -1;
  child_sync->load_status = false;
  child_sync->is_alive = true;
  child_sync->parent_alive = true;
  child_sync->waited = false;
  sema_init (&child_sync->sema_load, 0);
  sema_init (&child_sync->sema_wait, 0);
  lock_init (&child_sync->sync_lock);
  list_push_back (&thread_current ()->child_list, &child_sync->elem);

  /* Wrap startup arguments for the spawned thread */
  struct spawn_payload *sp = malloc (sizeof (struct spawn_payload));
  if (sp == NULL)
    {
      list_remove (&child_sync->elem);
      free (child_sync);
      palloc_free_page (name_copy);
      palloc_free_page (fn_copy);
      return TID_ERROR;
    }
  sp->fn_copy = fn_copy;
  sp->sync = child_sync;

  /* Create a new thread to execute FILE_NAME. */
  tid = thread_create (prog_name, PRI_DEFAULT, start_process, sp);
  palloc_free_page (name_copy);

  if (tid == TID_ERROR)
    {
      list_remove (&child_sync->elem);
      free (child_sync);
      free (sp);
      palloc_free_page (fn_copy);
      return TID_ERROR;
    }

  child_sync->tid = tid;

  /* Block until child has loaded its executable image */
  sema_down (&child_sync->sema_load);

  if (!child_sync->load_status)
    {
      list_remove (&child_sync->elem);
      free (child_sync);
      return TID_ERROR;
    }

  return tid;
}

/* A thread function that loads a user process and starts it running. */
static void
start_process (void *aux)
{
  struct spawn_payload *sp = (struct spawn_payload *) aux;
  char *file_name = sp->fn_copy;
  struct proc_sync *sync = sp->sync;
  free (sp);

  struct thread *cur = thread_current ();
  cur->sync_node = sync;

  struct intr_frame if_;
  bool success;

  /* Initialize interrupt frame and load executable. */
  memset (&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;
  success = load (file_name, &if_.eip, &if_.esp);

  /* Signal load status back to parent */
  if (!success)
    {
      sync->load_status = false;
      cur->sync_node = NULL;
      sema_up (&sync->sema_load);
      palloc_free_page (file_name);
      thread_exit ();
    }

  sync->load_status = true;
  sema_up (&sync->sema_load);
  palloc_free_page (file_name);

  /* Start the user process by simulating a return from an interrupt */
  asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory");
  NOT_REACHED ();
}

/* Waits for thread TID to die and returns its exit status.
   Returns -1 if already waited on or not a direct child. */
int
process_wait (tid_t child_tid) 
{
  struct thread *cur = thread_current ();
  struct list_elem *e;

  for (e = list_begin (&cur->child_list); e != list_end (&cur->child_list); e = list_next (e))
    {
      struct proc_sync *sync = list_entry (e, struct proc_sync, elem);
      if (sync->tid == child_tid)
        {
          if (sync->waited)
            return -1;
          sync->waited = true;

          sema_down (&sync->sema_wait);
          int exit_code = sync->exit_code;

          list_remove (&sync->elem);
          free (sync);

          return exit_code;
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

  /* 1. Print formatted exit status */
  if (cur->sync_node != NULL)
    printf ("%s: exit(%d)\n", cur->name, cur->sync_node->exit_code);

  /* 2. Close all open file descriptors */
  filesys_lock_acquire ();
  while (!list_empty (&cur->open_files))
    {
      struct list_elem *e = list_pop_front (&cur->open_files);
      struct fd_entry *entry = list_entry (e, struct fd_entry, elem);
      file_close (entry->file);
      free (entry);
    }

  /* 3. Re-enable write permissions on executable and close it */
  if (cur->executable != NULL)
    {
      file_allow_write (cur->executable);
      file_close (cur->executable);
      cur->executable = NULL;
    }
  filesys_lock_release ();

  /* 4. Update sync node for parent and free if parent already died */
  if (cur->sync_node != NULL)
    {
      struct proc_sync *sync = cur->sync_node;
      lock_acquire (&sync->sync_lock);
      sync->is_alive = false;
      sema_up (&sync->sema_wait);
      bool orphan = !sync->parent_alive;
      lock_release (&sync->sync_lock);

      if (orphan)
        free (sync);
    }

  /* 5. Free finished child records or mark running children as orphaned */
  while (!list_empty (&cur->child_list))
    {
      struct list_elem *e = list_pop_front (&cur->child_list);
      struct proc_sync *child_sync = list_entry (e, struct proc_sync, elem);

      lock_acquire (&child_sync->sync_lock);
      child_sync->parent_alive = false;
      bool dead = !child_sync->is_alive;
      lock_release (&child_sync->sync_lock);

      if (dead)
        free (child_sync);
    }

  /* 6. Destroy the process's page directory */
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

/* Pushes command-line arguments onto the user stack following 80x86 conventions */
static bool
push_command_args (const char *cmd_line, void **esp)
{
  char *cmd_copy = palloc_get_page (0);
  if (cmd_copy == NULL)
    return false;
  strlcpy (cmd_copy, cmd_line, PGSIZE);

  char *argv[128];
  int argc = 0;
  char *token, *save_ptr;

  for (token = strtok_r (cmd_copy, " ", &save_ptr); token != NULL;
       token = strtok_r (NULL, " ", &save_ptr))
    {
      argv[argc++] = token;
      if (argc >= 128)
        break;
    }

  uintptr_t argv_addr[128];

  /* 1. Push actual string tokens in reverse order */
  for (int i = argc - 1; i >= 0; i--)
    {
      size_t len = strlen (argv[i]) + 1;
      *esp = (void *) ((uint8_t *) *esp - len);
      memcpy (*esp, argv[i], len);
      argv_addr[i] = (uintptr_t) *esp;
    }

  /* 2. Word-align stack pointer to 4 bytes */
  uintptr_t align = ((uintptr_t) *esp) % 4;
  if (align != 0)
    {
      *esp = (void *) ((uint8_t *) *esp - align);
      memset (*esp, 0, align);
    }

  /* 3. Push null sentinel for argv[argc] */
  *esp = (void *) ((uint8_t *) *esp - sizeof (char *));
  *(char **) *esp = NULL;

  /* 4. Push pointers to each argument string in reverse order */
  for (int i = argc - 1; i >= 0; i--)
    {
      *esp = (void *) ((uint8_t *) *esp - sizeof (char *));
      *(char **) *esp = (char *) argv_addr[i];
    }

  /* 5. Push argv address (address of argv[0]) */
  char **argv_head = (char **) *esp;
  *esp = (void *) ((uint8_t *) *esp - sizeof (char **));
  *(char ***) *esp = argv_head;

  /* 6. Push argc count */
  *esp = (void *) ((uint8_t *) *esp - sizeof (int));
  *(int *) *esp = argc;

  /* 7. Push fake return address (NULL) */
  *esp = (void *) ((uint8_t *) *esp - sizeof (void *));
  *(void **) *esp = NULL;

  palloc_free_page (cmd_copy);

  /* Ensure the stack page was not overflowed */
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

  /* Extract the binary name from the command line */
  char prog_name[128];
  char *save_ptr;
  char *fn_temp = palloc_get_page (0);
  if (fn_temp == NULL)
    return false;
  strlcpy (fn_temp, file_name, PGSIZE);
  char *token = strtok_r (fn_temp, " ", &save_ptr);
  if (token != NULL)
    strlcpy (prog_name, token, sizeof prog_name);
  else
    prog_name[0] = '\0';
  palloc_free_page (fn_temp);

  /* Allocate and activate page directory. */
  t->pagedir = pagedir_create ();
  if (t->pagedir == NULL) 
    return false;
  process_activate ();

  /* Open executable file under synchronization lock */
  filesys_lock_acquire ();
  file = filesys_open (prog_name);
  if (file == NULL) 
    {
      filesys_lock_release ();
      printf ("load: %s: open failed\n", prog_name);
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
      printf ("load: %s: error loading executable\n", prog_name);
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

  /* Set up stack. */
  if (!setup_stack (esp))
    goto done;

  /* Push command line arguments onto the user stack */
  if (!push_command_args (file_name, esp))
    goto done;

  /* Start address. */
  *eip = (void (*) (void)) ehdr.e_entry;

  /* Prevent concurrent writes to running binary (ROX) */
  file_deny_write (file);
  t->executable = file;

  success = true;

 done:
  if (!success && file != NULL)
    file_close (file);
  filesys_lock_release ();
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
