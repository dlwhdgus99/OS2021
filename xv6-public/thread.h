#ifndef __THREAD__
#define __THREAD__

struct lwp{
  struct proc *proc;
  struct lwp *next;
};

struct lwp_group{
  struct lwp *front;
  struct lwp *back;
  struct lwp *cur;
  int count;
  int sz;
  int ticks;
  int quantum;
  int allotment;
  float pass;
  float stride;
  float ticket; 
};

void
initLwpGroup(struct lwp_group *lwps);

void
createLwpNode(struct proc *p, struct lwp *lwp);

void
appendLwpNode(struct lwp_group *lwps, struct lwp *lwp);

struct lwp *
removeLwpNode(struct lwp_group *lwps, int pid);

struct lwp_group *
createLwpGroup(struct lwp_group *lwps, struct lwp *lwp_node, struct proc *p);

struct lwp *
findNextLwpToRun(struct lwp_group *lwps);

struct lwp *
selectOtherMaster(struct proc *currentMaster);

#endif
