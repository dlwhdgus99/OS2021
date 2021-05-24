#include "thread.h"
#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

void
initLwpGroup(struct lwp_group *lwps)
{
  lwps->front = 0;
  lwps->back = 0; 
  lwps->cur = 0;
  lwps->count = 0;
  lwps->sz = 0;
  lwps->ticks = 0;
  lwps->quantum = 5;
  lwps->allotment = 20;
  lwps->pass = 0;
  lwps->stride = 0; 
  lwps->ticket = 0;
}

void
createLwpNode(struct proc *p, struct lwp *lwp)
{
  lwp->proc = p;
  lwp->next = 0;
}

void
appendLwpNode(struct lwp_group *lwps, struct lwp *lwp)
{
  lwp->next = 0; 
  if(lwps->count == 0)
  {
    lwps->front = lwp;
    lwps->cur = lwp;
  }
  else
  {
    lwps->back->next = lwp;
  }
  lwps->back = lwp;
  lwps->count++;
}

struct lwp *
removeLwpNode(struct lwp_group *lwps, int pid)
{
  if(lwps->count == 0){
    return 0;
  }

  struct lwp *lwp = lwps->front;
  if(lwps->count == 1){
    if(lwp->proc->pid == pid){
      lwps->front = 0;
      lwps->back = 0;
      lwps->cur = 0;
      lwps->count--;
      return lwp;
    }
    return 0;
  }
  else{
    if(lwp->proc->pid == pid){

      if(lwps->cur == lwp){
	if(lwp->next == 0)
	  lwps->cur = lwps->front;
	else
	  lwps->cur = lwp->next;
      }

      lwps->front = lwp->next;
      lwps->count--;
      return lwp;
    }

    struct lwp *ret = 0;
    while(1){
      if(lwp->next->proc->pid == pid){

	if(lwps->cur == lwp->next){
          if(lwp->next->next == 0)
            lwps->cur = lwps->front;
          else
            lwps->cur = lwp->next->next;
        }

        ret = lwp->next;
        lwp->next = lwp->next->next;
        if(lwp->next == 0) lwps->back = lwp;
        lwps->count--;
        return ret;
      }
      lwp = lwp->next;
      if(lwp->next == 0) return 0;
    }
    return 0;
  }
}

struct lwp_group *
createLwpGroup(struct lwp_group *lwps, struct lwp *lwp_node, struct proc *p)
{
  initLwpGroup(lwps);

  createLwpNode(p, lwp_node);
  appendLwpNode(lwps, lwp_node);

  p->my_lwp_group = lwps;
  p->ismaster = 1;

  return lwps;
}

//In the specific lwp group, find the next runnable lwp to run.
//lwps->cur is saved in the sched.
struct lwp *
findNextLwpToRun(struct lwp_group *lwps)
{
  if(!lwps)
    return 0;

  struct lwp *lwp = lwps->cur;
  int found_lwp = 0;

  for(;;){
    lwp = lwp->next;
    if(lwp == 0) {
      lwp = lwps->front;
    }

    if(lwp->proc->state == RUNNABLE){
      found_lwp = 1;
      break;
    }

    if(lwp == lwps->cur){
      break;
    }
  }
  if(found_lwp)
    return lwp;
  else
    return 0;
}   

struct lwp *
selectOtherMaster(struct proc *currentMaster)
{
  struct lwp_group *lwps = currentMaster->my_lwp_group;
  struct lwp *lwp;
  struct lwp *newMaster;

  for(lwp = lwps->front; lwp; lwp = lwp->next){
    if(lwp->proc == currentMaster)
      break;
  }

  if(lwp->next == 0)
    newMaster = lwps->front;
  else
    newMaster = lwp->next;

  newMaster->proc->ismaster = 1;
  newMaster->proc->pass = lwp->proc->pass;
  newMaster->proc->stride = lwp->proc->pass;
  newMaster->proc->ticket = lwp->proc->ticket;

  return newMaster;
}
