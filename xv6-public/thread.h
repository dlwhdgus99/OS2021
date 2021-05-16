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
};

void
initLwpGroup(struct lwp_group *lwps);

void
createLwpNode(struct proc *p, struct lwp *lwp);

void
appendLwpNode(struct lwp_group *lwps, struct lwp *lwp);

struct lwp *
removeLwpNode(struct lwp_group *lwps, int pid);

#endif
