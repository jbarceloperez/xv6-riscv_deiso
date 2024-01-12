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

struct devsw devsw[NDEV];
struct {
  struct spinlock lock;
  struct file file[NFILE];
} ftable;

void
fileinit(void)
{
  initlock(&ftable.lock, "ftable");
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

  // Encontrar struct file
  struct file * f = p->ofile[fd];
  // Es el fichero válido? 
  if( f->type != FD_INODE ) return (void *) -1;
  
  if( (prot & PROT_READ) && !(f->readable) ) return (void *) -1; 
  if( (prot & PROT_WRITE) && (flags & MAP_SHARED) && !(f->writable) ) {
	return (void *)-1;
  }

  if( (prot & PROT_READ) && (prot & PROT_WRITE))
    vma->state = VMA_RW;
  else if( (prot & PROT_READ) )
    vma->state = VMA_R;
  else
    panic("mmap: No read protection.");
  
  // Aumentar número de referencias al fichero 
  f = filedup(f);
  
  vma->f = f;
  vma->size = length;
  vma->file_init = offset;
  vma->shared = (flags & MAP_SHARED) ? 1 : 0;

  // Elegir posición inicial en memoria virtual  
  acquire(&ftable.lock);

  p->vma_ptr -= length;
  vma->init = p->vma_ptr;

  release(&ftable.lock);

  return (void *) (vma->init);
}

int write_in_center(struct file *f, uint64 addr, uint64 init, uint64 n){
    uint64 max = BSIZE; //((MAXOPBLOCKS-1-1-2) / 2) * BSIZE;
    uint64 i = 0;
    int r = 0;
    printf("\n%d %d %d\n", addr, init, n);
    while(i < n){
      printf("%d ", i);
      int n1 = n - i;
      if(n1 > max)
        n1 = max;

      begin_op();
      ilock(f->ip);
      
      r = writei(f->ip, 1, addr + i, init + i, n1);
      
      iunlock(f->ip);
      end_op();

      if(r != n1){
        printf("Probablemente el bloque %p no estaba mapeado. %d %d\n", addr + i, n1, r);
        i += n1;
      }
      else {
        i += r;
      }
      
    }
    int ret = (i == n ? n : -1);

  printf("Escritos: %d bytes del fichero en posición %d\n", ret, init);
  return ret;
}

void
uvmunmap_aux(pagetable_t pagetable, uint64 va, uint64 npages, int do_free){
  uint64 a;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  for(a = va; a < va + npages*PGSIZE; a += PGSIZE){
    if((pte = walk(pagetable, a, 0)) == 0)
      panic("uvmunmap: walk");
    if((*pte & PTE_V) != 0){
      uvmunmap(pagetable, va, npages, do_free);
    }
  }
}

int munmap(void *addr, int length){
  struct proc * p = myproc();
  // Encontrar VMAdata vacía en la lista de VMAdatas del proceso
  struct VMAdata * vma = 0;
  // addr dirección de una VMA ? 
  
  uint64 dir = (uint64) addr;
  for(int i = 0; i < NUM_VMA; ++i){
    
    if(p->vma[i].state != VMA_UNUSED            &&
       dir >= p->vma[i].init                    && 
       dir < p->vma[i].init + p->vma[i].size)
    {
      vma = &p->vma[i];
    }
  }

  if(0 == vma) {
    printf("munmap: addr not in VMA\n");
    setkilled(p);
    return -1;
  }

  // Hueco en medio.
  if(vma->init < dir && dir + length < vma->init + vma->size){
    return -1;
  }

  // VMA entera.
  if(vma->init == dir && dir + length >= vma->init + vma->size){
    if(vma->shared){
      if(-1 == write_in_center(vma->f, vma->init, vma->file_init, vma->size)){
        printf("%p %p %p %p\n", dir, vma->init, length, vma->size);	
        panic("munmap: filewrite 1 failed");
        return -1;
      }
    }
    uvmunmap_aux(p->pagetable, vma->init, vma->size / PGSIZE, 1);
    fileclose(vma->f);
    vma->state = VMA_UNUSED;
  }
  // VMA quitar final.
  else if(vma->init < dir && dir + length >= vma->init + vma->size){
    if(vma->shared){
      if(-1 == write_in_center(vma->f, dir, vma->file_init - vma->init + dir
	,(vma->init + vma->size) - dir)){
	printf("%p %p %p\n", dir, vma->init + vma->size - dir, length);
	panic("munmap: filewrite 2 failed");
        return -1;
      }
    }
    uvmunmap_aux(p->pagetable, 
	              dir, 
		      (vma->init + vma->size - dir) / PGSIZE, 
                      1);
    vma->size = dir - vma->init;
  }
  // VMA quitar inicio.
  else if(vma->init == dir && dir + length < vma->init + vma->size){
    if(vma->shared){
      if(-1 == write_in_center(vma->f, vma->init, vma->file_init, length)){
	panic("munmap: filewrite 3 failed");
	return -1;
      }
    }
    printf("%d", length /PGSIZE );
    uvmunmap_aux(p->pagetable, vma->init, length / PGSIZE, 1);
    
    vma->size = vma->size - length;
    vma->init = vma->init + length;
    vma->file_init = vma->file_init + length;
  } else {
    return -1;
  }
  
  return 0;
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
  
  printf("Lazy VMA, copiando fichero, copiados %d bytes, rellenando con ceros %d bytes. PID: %d, DIR: %p\n", 
	r, PGSIZE - r, myproc()->pid, dir);
  printf("... vma.init: %p, vma.size: %d, vma.file_init: %d\n", 
        vma->init, vma->size, vma->file_init);
  if(r < PGSIZE){
    char src = 0;
    for(uint64 aux = dir + r;  aux < dir + PGSIZE; aux += 1){
      //printf("%p ", aux);
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

