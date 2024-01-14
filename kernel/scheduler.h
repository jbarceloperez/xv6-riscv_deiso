// TAREA 1
#ifndef SCHEDULER
#define SCHEDULER

// MAX_PROC debe ser potencia de 2
struct random_generator{
  long last_gen;
  long P;
  long seed; 
};

struct scheduler{
  //struct proc * proc[MAX_PROC];
  //int segtrees[2*MAX_PROC];
  int num_proc;                 // numero de procesos en uso
  int sum_tickets;              // suma de todos los tickets repartidos entre todos los procesos
  struct spinlock lock;         // cerrojo para exclusión mutua del planificador
  struct random_generator rg;   
};

struct proc * GetProc();

int scheduler_addproc(struct proc * proc);

int scheduler_settickets(struct proc * proc, int tickets);

int scheduler_removeproc(struct proc * proc);

struct proc * scheduler_nextproc();

void initscheduler();

#endif //SCHEDULER
