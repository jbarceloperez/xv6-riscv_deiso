//
// Support functions for system calls that involve file descriptors.
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "fs.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "file.h"
#include "stat.h"
#include "proc.h"

// TAREA 2
#include "memlayout.h"

struct devsw devsw[NDEV];
struct {
  struct spinlock lock;
  struct file file[NFILE];
  // TAREA 2
  uint64 vma_ptr;
} ftable;

void
fileinit(void)
{
  initlock(&ftable.lock, "ftable");
  // TAREA 2
  ftable.vma_ptr = VMA_INIT;
}

// Allocate a file structure.
struct file*
filealloc(void)
{
  struct file *f;

  acquire(&ftable.lock);
  for(f = ftable.file; f < ftable.file + NFILE; f++){
    if(f->ref == 0){
      f->ref = 1;
      release(&ftable.lock);
      return f;
    }
  }
  release(&ftable.lock);
  return 0;
}

// Increment ref count for file f.
struct file*
filedup(struct file *f)
{
  acquire(&ftable.lock);
  if(f->ref < 1)
    panic("filedup");
  f->ref++;
  release(&ftable.lock);
  return f;
}

// Close file f.  (Decrement ref count, close when reaches 0.)
void
fileclose(struct file *f)
{
  struct file ff;

  acquire(&ftable.lock);
  if(f->ref < 1)
    panic("fileclose");
  if(--f->ref > 0){
    release(&ftable.lock);
    return;
  }
  ff = *f;
  f->ref = 0;
  f->type = FD_NONE;
  release(&ftable.lock);

  if(ff.type == FD_PIPE){
    pipeclose(ff.pipe, ff.writable);
  } else if(ff.type == FD_INODE || ff.type == FD_DEVICE){
    begin_op();
    iput(ff.ip);
    end_op();
  }
}

// Get metadata about file f.
// addr is a user virtual address, pointing to a struct stat.
int
filestat(struct file *f, uint64 addr)
{
  struct proc *p = myproc();
  struct stat st;
  
  if(f->type == FD_INODE || f->type == FD_DEVICE){
    ilock(f->ip);
    stati(f->ip, &st);
    iunlock(f->ip);
    if(copyout(p->pagetable, addr, (char *)&st, sizeof(st)) < 0)
      return -1;
    return 0;
  }
  return -1;
}

// Read from file f.
// addr is a user virtual address.
int
fileread(struct file *f, uint64 addr, int n)
{
  int r = 0;

  if(f->readable == 0)
    return -1;

  if(f->type == FD_PIPE){
    r = piperead(f->pipe, addr, n);
  } else if(f->type == FD_DEVICE){
    if(f->major < 0 || f->major >= NDEV || !devsw[f->major].read)
      return -1;
    r = devsw[f->major].read(1, addr, n);
  } else if(f->type == FD_INODE){
    ilock(f->ip);
    if((r = readi(f->ip, 1, addr, f->off, n)) > 0)
      f->off += r;
    iunlock(f->ip);
  } else {
    panic("fileread");
  }

  return r;
}

// Write to file f.
// addr is a user virtual address.
int
filewrite(struct file *f, uint64 addr, int n)
{
  int r, ret = 0;

  if(f->writable == 0)
    return -1;

  if(f->type == FD_PIPE){
    ret = pipewrite(f->pipe, addr, n);
  } else if(f->type == FD_DEVICE){
    if(f->major < 0 || f->major >= NDEV || !devsw[f->major].write)
      return -1;
    ret = devsw[f->major].write(1, addr, n);
  } else if(f->type == FD_INODE){
    // write a few blocks at a time to avoid exceeding
    // the maximum log transaction size, including
    // i-node, indirect block, allocation blocks,
    // and 2 blocks of slop for non-aligned writes.
    // this really belongs lower down, since writei()
    // might be writing a device like the console.
    int max = ((MAXOPBLOCKS-1-1-2) / 2) * BSIZE;
    int i = 0;
    while(i < n){
      int n1 = n - i;
      if(n1 > max)
        n1 = max;

      begin_op();
      ilock(f->ip);
      if ((r = writei(f->ip, 1, addr + i, f->off, n1)) > 0)
        f->off += r;
      iunlock(f->ip);
      end_op();

      if(r != n1){
        // error from writei
        break;
      }
      i += r;
    }
    ret = (i == n ? n : -1);
  } else {
    panic("filewrite");
  }

  return ret;
}

// TAREA 2
void * mmap(void *addr, int length, int prot, int flags,
           int fd, int offset){
  struct proc * p = myproc();
  // Encontrar VMAdata vacía en la lista de VMAdatas del proceso
  struct VMAdata * vma = 0;
  for(int i = 0; i < NUM_VMA; ++i){
    if(p->vma[i].state == VMA_UNUSED) {
      vma = &(p->vma[i]);
      break;
    }
  }

  if(0 == vma) panic("mmap: No VMA available.");

  if( (prot & PROT_READ) && (prot & PROT_WRITE))
    vma->state = VMA_RW;
  else if( (prot & PROT_READ) )
    vma->state = VMA_R;
  else
    panic("mmap: No read protection.");
  // Encontrar struct file
  struct file * f = p->ofile[fd];
  // Aumentar número de referencias al fichero 
  filedup(f);
  
  vma->f = f;
  vma->size = length;
  vma->file_init = offset;
  vma->shared = (flags & MAP_SHARED) ? 1 : 0;

  // Elegir posición inicial en memoria virtual  
  acquire(&ftable.lock);

  ftable.vma_ptr -= length;
  vma->init = ftable.vma_ptr;

  release(&ftable.lock);

  return (void *) (vma->init);
}

int munmap(void *addr, int length){
	return -1;
}

int load_file_page(struct VMAdata * vma, uint64 dir){
	
  if(dir % PGSIZE != 0){
    struct proc * p = myproc();
    printf("VMA lazy load failed, dir not aligned with page size.\n");
    printf("usertrap(): unexpected scause %p pid=%d\n", r_scause(), p->pid);
    printf("            sepc=%p stval=%p\n", r_sepc(), r_stval());
    setkilled(p);
    return -1;
  }  

  ilock(vma->f->ip);
  
  // Leer una única página.

  int r = readi(vma->f->ip, 
                         1, 
                       dir, // destino
           dir - vma->init + vma->file_init, // offset
                    PGSIZE);

  iunlock(vma->f->ip);

  if(r < 0){
    struct proc * p = myproc();
    printf("VMA lazy load failed, file readi failed.\n");
    printf("usertrap(): unexpected scause %p pid=%d\n", r_scause(), p->pid);
    printf("            sepc=%p stval=%p\n", r_sepc(), r_stval());
    setkilled(p);
    return -1;	
  }
  
  if(r < PGSIZE){
    char src = 0;
    for(uint64 aux = dir + r;  aux < dir + PGSIZE; aux += 1){
	
      if(-1 == either_copyout(1, 
		     aux, 
                     &src, 
                     1)){
	struct proc * p = myproc();
        printf("VMA lazy load failed, fallo rellenando con ceros.\n");
        printf("usertrap(): unexpected scause %p pid=%d\n", r_scause(), p->pid);
        printf("            sepc=%p stval=%p\n", r_sepc(), r_stval());
        setkilled(p);
        return -1;	
      } 
    }
  }
  
  return 0;
}

