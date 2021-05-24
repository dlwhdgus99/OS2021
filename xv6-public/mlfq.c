#include "queue.h"
#include "mlfq.h"

void
initmlfq(struct mlfq *mlfq, struct queue *h, struct queue *m, struct queue *l)
{
  mlfq->hqueue = h;
  mlfq->mqueue = m;
  mlfq->lqueue = l;

  mlfq->totalticks = 0;
}

void
pushToMlfq(struct mlfq *mlfq, struct node *node, struct proc *p)
{
  createnode(p, node);
  push(mlfq->hqueue, node);
  if(mlfq->hqueue->count == 1)
    mlfq->hqueue->cur = node;
}

