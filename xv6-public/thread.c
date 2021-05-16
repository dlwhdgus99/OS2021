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
  //n->next = 0; 
  if(lwps->count == 0)
  {
    lwps->front = lwp;
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
      lwps->count--;
      return lwp;
    }
    return 0;
  }
  else{
    if(lwp->proc->pid == pid){
      lwps->front = lwp->next;
      lwps->count--;
      return lwp;
    }
    struct lwp *ret = 0;
    while(1){
      if(lwp->next->proc->pid == pid){
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
