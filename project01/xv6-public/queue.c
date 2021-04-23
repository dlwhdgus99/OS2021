#include "queue.h"
#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

void
initqueue(struct queue *q, int quant, int allot)
{
  q->front = 0; 
  q->back = 0;
  q->quantum = quant;
  q->allotment = allot;
  q->cur = 0;
  q->count = 0;
}

void
createnode(struct proc *p, struct node *n)
{
  n->proc = p;
  n->next = 0;
}

void
push(struct queue *q, struct node *n)
{
  if(q->count == 0)
  {
    q->front = n;
  }
  else
  {
    if(q->back == 0){
      cprintf("back is null..\n");
    }
    q->back->next = n;
  }
  q->back = n;
  q->count++;
}

struct node *
pop(struct queue *q, struct proc *p)
{
  if(q->count == 0){
    return 0;
  }

  struct node *node = q->front;
  if(q->count == 1){
    if(node->proc->pid == p->pid){
      q->front = 0;
      q->back = 0;
      q->count--;
      if(node == 0){
	cprintf("pop null\n");
      }
      return node;
    }
    return 0;
  }
  else{
    if(node->proc->pid == p->pid){
      q->front = node->next;
      q->count--;
      return node;
    }
    while(1){
      if(node->next->proc->pid == p->pid){
        struct node *ret = node->next;
        node->next = node->next->next;
        if(node->next == 0) q->back = node;
        q->count--;
        return ret;
      }
      node = node->next;
      if(node->next == 0) return 0;
    }
    return 0;
  }
}
