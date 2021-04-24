#include "priqueue.h"
#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

void
init_priqueue(struct priority_queue *pq)
{
  pq->count = 0;  
}

void 
pq_push(struct priority_queue *pq, struct proc *p)
{
  if(pq->count >= NPROC){
     return ;
  }
  
  pq->heap[pq->count] = p;

  //cprintf("push index: %d, pid: %d\n", pq->count, p->pid);

  int now = pq->count;
  int parent = (pq->count-1)/2;

  while(now > 0 && pq->heap[now]->pass < pq->heap[parent]->pass){
    struct proc *temp = pq->heap[now];
    pq->heap[now] = pq->heap[parent];
    pq->heap[parent] = temp;
    now = parent;
    parent = (parent-1)/2;
  }
  pq->count++;
}

struct proc *
pq_pop(struct priority_queue *pq)
{
  if(pq->count <= 0){
    return (struct proc *)-1;
  }

  struct proc *p = pq->heap[0];

  int now = 0;
  int lchild = 1;
  int rchild = 2;
  int target = now;

  pq->count--;
  pq->heap[0] = pq->heap[pq->count];
  
  //cprintf("pop pid: %d", pq->heap[0]->pid);

  while(lchild < pq->count){
    if(pq->heap[target]->pass > pq->heap[lchild]->pass){
      target = lchild;
    }
    else if(pq->heap[target]->pass > pq->heap[rchild]->pass
	 && rchild < pq->count){
      target = rchild;
    }
    if(target == now){
      break;
    }
    else{
      struct proc *temp = pq->heap[now];
      pq->heap[now] = pq->heap[target];
      pq->heap[target] = temp;
      now = target;
      lchild = now * 2 + 1;
      rchild = now * 2 + 2;
    }
  }
  return p;
}

struct proc *
pq_select_pop(struct priority_queue *pq, int pid)
{
  struct proc* temp_proc[NPROC];
  int idx = 0;

  struct proc *p = pq_pop(pq);
  while(p->pid != pid){
    temp_proc[idx++] = p;
    p = pq_pop(pq);
  }
  for(int i = 0; i < idx; i++){
    pq_push(pq, temp_proc[i]); 
  }

  return p;
}
