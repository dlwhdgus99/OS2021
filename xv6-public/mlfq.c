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
