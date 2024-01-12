#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

#include "fs.h"
#include "sleeplock.h"
#include "file.h"
struct spinlock tickslock;
uint ticks;

extern char trampoline[], uservec[], userret[];

// in kernelvec.S, calls kerneltrap().
void kernelvec();

extern int devintr();

void
trapinit(void)
{
  initlock(&tickslock, "time");
}

// set up to take exceptions and traps while in the kernel.
void
trapinithart(void)
{
  w_stvec((uint64)kernelvec);
}

// TAREA 2
struct filepage_node{
  int next;
  int page;
  uint64 addr;
};
 
struct {
 struct spinlock lock;
 int first_free;
 struct filepage_node l[NUM_FILEPAGES];
} filepages;

void init_filepages(){
  initlock(&filepages.lock, "filepages");
  filepages.first_free = 0;
  for(int i = 0; i < NUM_FILEPAGES; ++i){
    filepages.l[i].next = i+1;
    filepages.l[i].page = -1;
    filepages.l[i].addr = 0;
  }
  filepages.l[NUM_FILEPAGES - 1].next = -1;
}

int get_node(){
  if(filepages.first_free == -1) panic("No quedan nodos para almacenar páginas.");
  
  int ret = filepages.first_free;
  
  filepages.first_free = filepages.l[ret].next;
  
  return ret;
}

int new_file(){
  acquire(&filepages.lock);

  int ret = get_node();
  filepages.l[ret].next = -1;
  filepages.l[ret].page = -1;
  filepages.l[ret].addr = 0;

  release(&filepages.lock);
  return ret; 
}

void add_page(int init_node, int page, uint64 addr){
  acquire(&filepages.lock);

  int new = get_node();
  filepages.l[new].next = filepages.l[init_node].next;
  filepages.l[init_node].next = new;
  filepages.l[new].page = page;
  filepages.l[new].addr = addr;
 
  release(&filepages.lock);
}

uint64 find_page(int init_node, int page){
  acquire(&filepages.lock);
  uint64 ret = 0;
  for(int i = init_node; i != -1; i = filepages.l[i].next){
    if(filepages.l[i].page == page){ 
      ret = filepages.l[i].addr;
      break;
    }
  }
  release(&filepages.lock);
  return ret;
}

void remove_page(int init_node, int page){
  acquire(&filepages.lock);
  for(int i = init_node; filepages.l[i].next != -1; i = filepages.l[i].next){
    struct filepage_node * next_node = &filepages.l[filepages.l[i].next];
    if(next_node->page == page){
      filepages.l[i].next = next_node->next;
      next_node->next = filepages.first_free;
      filepages.first_free = filepages.l[i].next;

      next_node->page = -1;
      next_node->addr = 0;
    }
  }
  release(&filepages.lock);
}


// TAREA 2
void load_page_if_correct(uint64 dir, int is_write){
  struct proc * p = myproc();  
  
  if(dir == 0) {
	printf("VMA lazy load failed, dir == 0\n");
        printf("usertrap(): unexpected scause %p pid=%d\n", r_scause(), p->pid);
    	printf("            sepc=%p stval=%p\n", r_sepc(), r_stval());
    	setkilled(p);
	return;
  }

  // dir dirección de una VMA ? 
  struct VMAdata * vma = 0;
  for(int i = 0; i < NUM_VMA; ++i){

    if(p->vma[i].state != VMA_UNUSED            &&
       dir >= p->vma[i].init                    && 
       dir < p->vma[i].init + p->vma[i].size)
    {
      vma = &p->vma[i];
    }
  }

  if(0 == vma) {
    printf("VMA lazy load failed, dir not in VMA\n");
    printf("usertrap(): unexpected scause %p pid=%d\n", r_scause(), p->pid);
    printf("            sepc=%p stval=%p\n", r_sepc(), r_stval());
    setkilled(p);
    return;
  }

  if(vma->state == VMA_R && is_write) {
    printf("VMA lazy load failed, VMA has no write permission.\n");
    printf("usertrap(): unexpected scause %p pid=%d\n", r_scause(), p->pid);
    printf("            sepc=%p stval=%p\n", r_sepc(), r_stval());
    setkilled(p);
    return;
  }
  
  //CARGAR PÁGINA SI: NO ESTÁ YA EN MEMORIA 
  //                  SI ES VMA_RW Y VMA_PRIVATE
  //SI ES COMPARTIDA AÑADIR A UNA TABLA PARA QUE LA ENCUENTREN FUTUROS PROCESOS 
  //Para indexar la tabla, añadimos un campo a struct file que indica la posición
  //en la tabla, en la tabla podemos almacenar una lista de segmentos contiguos 
  //de páginas reservadas, si no dejamos huecos en la lista, es bastante eficiente. 
  int shared = (vma->shared) ? 1 : 0; 
  
  uint64 pa = 0;
  int page = (vma->file_init + (dir - vma->init)) / PGSIZE;
  if( shared ){
    int pos = vma->f->pos_in_filepages;
    if(pos == -1){
      vma->f->pos_in_filepages = pos = new_file();
    }  
    pa = find_page(pos, page);
     
  }
  
  int load = 0;
  if( !shared || pa == 0){
    uint64 pa = (uint64) kalloc();
    if(0 == pa) {
      printf("VMA lazy load failed because kalloc failed.\n");
      printf("usertrap(): unexpected scause %p pid=%d\n", r_scause(), p->pid);
      printf("            sepc=%p stval=%p\n", r_sepc(), r_stval());
      setkilled(p);
    return;
    }
    load = 1;
    if( shared ){
      add_page(vma->f->pos_in_filepages, page, pa);
    }
  }

  printf("Usertrap: lazy VMA, PID: %d, DIR: %p\n", p->pid, dir);

  incref((void *)pa); 
  mappages(p->pagetable, 
                        dir, 
                     PGSIZE, 
                         pa, 
               PTE_V|PTE_U|PTE_R| ((vma->state == VMA_RW) ? PTE_W : 0) );

  // ÉXITO, COPIAR CONTENIDO
  if( load ) {
    load_file_page(vma, dir);
  }

  // Tratar res == -1 (fallo?)
}

//
// handle an interrupt, exception, or system call from user space.
// called from trampoline.S
//
void
usertrap(void)
{
  int which_dev = 0;

  if((r_sstatus() & SSTATUS_SPP) != 0)
    panic("usertrap: not from user mode");

  // send interrupts and exceptions to kerneltrap(),
  // since we're now in the kernel.
  w_stvec((uint64)kernelvec);

  struct proc *p = myproc();
  
  // save user program counter.
  p->trapframe->epc = r_sepc();
  
  if(r_scause() == 8){
    // system call

    if(killed(p))
      exit(-1);

    // sepc points to the ecall instruction,
    // but we want to return to the next instruction.
    p->trapframe->epc += 4;

    // an interrupt will change sepc, scause, and sstatus,
    // so enable only now that we're done with those registers.
    intr_on();

    syscall();	
  } else if((which_dev = devintr()) != 0){
    // ok
  } else if (r_scause() == 13 )  // Fallo de página en lectura TAREA 2
  {
    load_page_if_correct(r_stval(), 0);
  } else if (r_scause() == 15 )  // Fallo de página en escritura TAREA 2
  {
    load_page_if_correct(r_stval(), 1); 
  } else {
    printf("usertrap(): unexpected scause %p pid=%d\n", r_scause(), p->pid);
    printf("            sepc=%p stval=%p\n", r_sepc(), r_stval());
    setkilled(p);
  }

  if(killed(p))
    exit(-1);

  // give up the CPU if this is a timer interrupt.
  if(which_dev == 2)
    yield();

  usertrapret();
}

//
// return to user space
//
void
usertrapret(void)
{
  struct proc *p = myproc();

  // we're about to switch the destination of traps from
  // kerneltrap() to usertrap(), so turn off interrupts until
  // we're back in user space, where usertrap() is correct.
  intr_off();

  // send syscalls, interrupts, and exceptions to uservec in trampoline.S
  uint64 trampoline_uservec = TRAMPOLINE + (uservec - trampoline);
  w_stvec(trampoline_uservec);

  // set up trapframe values that uservec will need when
  // the process next traps into the kernel.
  p->trapframe->kernel_satp = r_satp();         // kernel page table
  p->trapframe->kernel_sp = p->kstack + PGSIZE; // process's kernel stack
  p->trapframe->kernel_trap = (uint64)usertrap;
  p->trapframe->kernel_hartid = r_tp();         // hartid for cpuid()

  // set up the registers that trampoline.S's sret will use
  // to get to user space.
  
  // set S Previous Privilege mode to User.
  unsigned long x = r_sstatus();
  x &= ~SSTATUS_SPP; // clear SPP to 0 for user mode
  x |= SSTATUS_SPIE; // enable interrupts in user mode
  w_sstatus(x);

  // set S Exception Program Counter to the saved user pc.
  w_sepc(p->trapframe->epc);

  // tell trampoline.S the user page table to switch to.
  uint64 satp = MAKE_SATP(p->pagetable);

  // jump to userret in trampoline.S at the top of memory, which 
  // switches to the user page table, restores user registers,
  // and switches to user mode with sret.
  uint64 trampoline_userret = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64))trampoline_userret)(satp);
}

// interrupts and exceptions from kernel code go here via kernelvec,
// on whatever the current kernel stack is.
void 
kerneltrap()
{
  int which_dev = 0;
  uint64 sepc = r_sepc();
  uint64 sstatus = r_sstatus();
  uint64 scause = r_scause();
  
  if((sstatus & SSTATUS_SPP) == 0)
    panic("kerneltrap: not from supervisor mode");
  if(intr_get() != 0)
    panic("kerneltrap: interrupts enabled");

  if((which_dev = devintr()) == 0){
    printf("scause %p\n", scause);
    printf("sepc=%p stval=%p\n", r_sepc(), r_stval());
    panic("kerneltrap");
  }

  // give up the CPU if this is a timer interrupt.
  if(which_dev == 2 && myproc() != 0 && myproc()->state == RUNNING)
    yield();

  // the yield() may have caused some traps to occur,
  // so restore trap registers for use by kernelvec.S's sepc instruction.
  w_sepc(sepc);
  w_sstatus(sstatus);
}

void
clockintr()
{
  acquire(&tickslock);
  ticks++;
  wakeup(&ticks);
  release(&tickslock);
}

// check if it's an external interrupt or software interrupt,
// and handle it.
// returns 2 if timer interrupt,
// 1 if other device,
// 0 if not recognized.
int
devintr()
{
  uint64 scause = r_scause();

  if((scause & 0x8000000000000000L) &&
     (scause & 0xff) == 9){
    // this is a supervisor external interrupt, via PLIC.

    // irq indicates which device interrupted.
    int irq = plic_claim();

    if(irq == UART0_IRQ){
      uartintr();
    } else if(irq == VIRTIO0_IRQ){
      virtio_disk_intr();
    } else if(irq){
      printf("unexpected interrupt irq=%d\n", irq);
    }

    // the PLIC allows each device to raise at most one
    // interrupt at a time; tell the PLIC the device is
    // now allowed to interrupt again.
    if(irq)
      plic_complete(irq);

    return 1;
  } else if(scause == 0x8000000000000001L){
    // software interrupt from a machine-mode timer interrupt,
    // forwarded by timervec in kernelvec.S.

    if(cpuid() == 0){
      clockintr();
    }
    
    // acknowledge the software interrupt by clearing
    // the SSIP bit in sip.
    w_sip(r_sip() & ~2);

    return 2;
  } else {
    return 0;
  }
}

