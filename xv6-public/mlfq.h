#include "queue.h"

#ifndef MLFQ
#define MLFQ

struct mlfq{
  struct queue *hqueue;
  struct queue *mqueue;
  struct queue *lqueue;
  int totalticks;
};

void
initmlfq(struct mlfq *mlfq, struct queue *h, struct queue *m, struct queue *l);

void
pushToMlfq(struct mlfq *mlfq, struct node *node, struct proc *p);

#endif
