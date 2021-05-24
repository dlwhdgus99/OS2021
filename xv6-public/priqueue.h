#include "param.h"

#ifndef PRIQUEUE
#define PRIQUEUE

struct priority_queue{
  struct proc *heap[NPROC];
  int count;
};

void
init_priqueue(struct priority_queue *pq);

void
pq_push(struct priority_queue *pq, struct proc *p);

struct proc *
pq_pop(struct priority_queue *pq);

struct proc *
pq_select_pop(struct priority_queue *pq, int pid);

void
init_mproc(struct proc *mproc);

#endif
