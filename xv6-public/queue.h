#include "param.h"

#ifndef QUEUE
#define QUEUE

struct node{
  struct proc *proc;
  struct node *next;
};

struct queue{
  struct node *front;
  struct node *back;
  struct node *cur;
  int quantum;
  int allotment;
  int count;
};

void
initqueue(struct queue *q, int quant, int allot);

void
createnode(struct proc *p, struct node *n);

void
push(struct queue *q, struct node *n);

struct node *
pop(struct queue *q, struct proc *p);

void
init_qcur(struct queue *q);

void
adjustCurForRoundRobin(struct queue *q, struct node *n);

#endif
