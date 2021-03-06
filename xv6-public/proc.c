#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"
#include "queue.h"
#include "mlfq.h"
#include "priqueue.h"
#include "thread.h"
#include "syscall.h"

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

struct mlfq mlfqueue;
struct queue high_queue;
struct queue mid_queue;
struct queue low_queue;

struct node node[NPROC];

struct lwp_group lwp_groups[NPROC];
struct lwp lwp_node[NPROC];

struct proc mlfq_proc;
struct priority_queue priqueue;

//struct proc lwp_group[NPROC];

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
}

// Must be called with interrupts disabled
int
cpuid() {
  return mycpu()-cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu*
mycpu(void)
{
  int apicid, i;
  
  if(readeflags()&FL_IF)
    panic("mycpu called with interrupts enabled\n");
  
  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i) {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc*
myproc(void) {
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);

  int i = 0;
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++, i++)
    if(p->state == UNUSED){
      p->ptable_idx = i;
      goto found;
    }

  cprintf("not found panic\n");
  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;

  release(&ptable.lock);

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    cprintf("kernel stack panic\n");
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;

  return p;
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];
  struct mlfq *mlfq = &mlfqueue;

  //pointer to the 3 level queues.
  struct queue *hqueue = &high_queue;
  struct queue *mqueue = &mid_queue;
  struct queue *lqueue = &low_queue;

  struct lwp_group *lwps;

  //initialize 3 level queues.
  initqueue(hqueue, 5,20);
  initqueue(mqueue, 10,40);
  initqueue(lqueue, 20,0);

  //initialize mlfq.
  initmlfq(mlfq, hqueue, mqueue, lqueue);

  p = allocproc();
  
  initproc = p;

  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

  p->state = RUNNABLE;
  p->tick = 0;
  p->mlfq_level = 0;
  p->ismlfq = 0;

  pushToMlfq(mlfq, &node[p->ptable_idx], p);

  lwps = createLwpGroup(&lwp_groups[p->ptable_idx], &lwp_node[p->ptable_idx], p);
  lwps->sz = PGSIZE;

  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint oldsz, newsz = 0;
  struct proc *curproc = myproc();
  struct lwp_group *lwps = curproc->my_lwp_group;
  struct lwp *lwp;

  acquire(&ptable.lock);

  oldsz = lwps->sz;

  if(n > 0){
    if((newsz = allocuvm(curproc->pgdir, oldsz, oldsz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((newsz = deallocuvm(curproc->pgdir, oldsz, oldsz + n)) == 0)
      return -1;
  }

  for(lwp = lwps->front; lwp; lwp = lwp->next)
    lwp->proc->sz = newsz;
  lwps->sz = newsz;
  switchuvm(curproc);

  release(&ptable.lock);
  return oldsz;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  int i, pid;
  uint newsz;
  struct proc *np;
  struct proc *curproc = myproc();
  struct mlfq *mlfq = &mlfqueue;
  struct lwp_group *lwps;

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy process state from proc.
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz, &newsz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }

  np->parent = curproc;
  *np->tf = *curproc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);

  np->state = RUNNABLE;

  //For scheduling
  np->tick = 0;
  np->mlfq_level = 0;
  np->ismlfq = 0;
  
  pushToMlfq(mlfq, &node[np->ptable_idx], np);

  lwps = createLwpGroup(&lwp_groups[np->ptable_idx], &lwp_node[np->ptable_idx], np);

  np->sz = newsz;
  lwps->sz = newsz;

  release(&ptable.lock);

  return pid;
}

void
popFromSchedulingQueue(struct proc *p)
{
  struct mlfq *mlfq = &mlfqueue;
  struct priority_queue *pq = &priqueue;
  struct proc *mproc = &mlfq_proc;
  struct node *n;

  if(p->ticket == 0){
    if(p->mlfq_level == 0){
      pop(mlfq->hqueue, p);
    }
    else if(p->mlfq_level == 1){
      n = pop(mlfq->mqueue, p);
      if(mlfq->mqueue->cur == n)
        adjustCurForRoundRobin(mlfq->mqueue, n);
    }
    else{
      n = pop(mlfq->lqueue, p);
      if(mlfq->lqueue->cur == n)
        adjustCurForRoundRobin(mlfq->lqueue, n);
    }
  }
  else{
    pq_select_pop(pq, p->pid);
    mproc->ticket += p->ticket;
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  struct lwp_group *lwps = curproc->my_lwp_group;
  struct lwp *lwp;
  int fd;

  if(curproc == initproc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
      fileclose(curproc->ofile[fd]);
      for(lwp = lwps->front; lwp; lwp = lwp->next){
        lwp->proc->ofile[fd] = 0;
      }
    }
  }

  begin_op();
  iput(curproc->cwd);
  end_op();
  for(lwp = lwps->front; lwp; lwp = lwp->next){
    lwp->proc->cwd = 0;
  }

  acquire(&ptable.lock);
  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for(lwp = lwps->front; lwp; lwp = lwp->next){
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent == lwp->proc){
        p->parent = initproc;
        if(p->state == ZOMBIE)
          wakeup1(initproc);
      }
    }
  }

  for(lwp = lwps->front; lwp; lwp = lwp->next){
    lwp->proc->state = ZOMBIE;
  }
  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

int
isAllZombie(struct lwp_group *lwps)
{
  struct lwp *lwp;

  for(lwp = lwps->front; lwp; lwp = lwp->next){
    if(lwp->proc->state != ZOMBIE)
      return 0;
  }
  return 1;
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  int havekids,  pid = 0;
  struct proc *curproc = myproc();
  struct lwp_group *lwps;
  struct lwp *lwp;

  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;

    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      havekids = 1;
      lwps = p->my_lwp_group;

      if(isAllZombie(lwps)){
        // Found one.

	for(lwp = lwps->front; lwp; lwp = lwp->next){
	  p = lwp->proc;

	  if(p->ismaster){
	    popFromSchedulingQueue(p);
	  }
	  p->ticket = 0;
	  p->sz = 0;
          p->ustack_va = 0;

	  removeLwpNode(lwps, p->pid);
          p->my_lwp_group = 0;

          pid = p->pid;
          kfree(p->kstack);
          p->kstack = 0;
          p->pid = 0;
          p->parent = 0;
          p->name[0] = 0;
          p->killed = 0;
          p->state = UNUSED;
	}
	lwps->sz = 0;
	lwps->quantum = 0;
	lwps->ticket = 0;
	freevm(p->pgdir);
	for(lwp = lwps->front; lwp; lwp = lwp->next){
	  p->pgdir = 0;
	}
      
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}

int
getlev(void)
{
  return myproc()->mlfq_level;
}

int
set_cpu_share(int share)
{
  struct proc *curproc = myproc();
  struct mlfq *mlfq = &mlfqueue;
  struct proc *mproc = &mlfq_proc;
  struct priority_queue *pq = &priqueue;
  struct lwp_group *lwps = curproc->my_lwp_group;
  struct lwp *lwp;
  struct proc *master;
  struct node *n;
 
  if(mproc->ticket - share < 20){
    return -1;
  }

  acquire(&ptable.lock);

  for(lwp = lwps->front; lwp; lwp = lwp->next)
    if(lwp->proc->ismaster)
      break;
  master = lwp->proc; 

  if(master->mlfq_level == 0){
    pop(mlfq->hqueue, master);
  }
  else if(master->mlfq_level == 1){
    n = pop(mlfq->mqueue, master);
    if(mlfq->mqueue->cur == n)
      adjustCurForRoundRobin(mlfq->mqueue, n);
  }
  else{
    n = pop(mlfq->lqueue, master);
    if(mlfq->lqueue->cur == n)
      adjustCurForRoundRobin(mlfq->lqueue, n);
  }
  mproc->ticket -= share;
  mproc->stride = LNUM/mproc->ticket;

  lwps->ticks = 0;
  lwps->quantum = 5;
  lwps->ticket = master->ticket = share;
  lwps->stride = master->stride = LNUM/master->ticket;
  lwps->pass = master->pass = pq->heap[0]->pass;
  master->ismlfq = 0;

  pq_push(pq, master); 

  release(&ptable.lock);
  return 0;
}

int
chkallot(struct queue *now,  struct queue *low)
{
  struct proc *p;  
  struct lwp_group *lwps = myproc()->my_lwp_group;
  struct lwp *lwp;
  struct node *n;

  if(lwps->ticks >= lwps->allotment){
    //find master thread.
    for(lwp = lwps->front; lwp; lwp = lwp->next){
      if(lwp->proc->ismaster){
        break;
      }
    }
    p = lwp->proc;

    n = pop(now, p);
    if(now->cur == n)
      adjustCurForRoundRobin(now, n);

    push(low, n);
    if(low->count == 1)
      init_qcur(low);

    lwps->ticks = 0;
    lwps->quantum = low->quantum;
    lwps->allotment = low->allotment;
    for(lwp = lwps->front; lwp; lwp = lwp->next){
      lwp->proc->mlfq_level++;
    }

    return 1;
  }
  return 0;
}

int
chkboost()
{
  struct node *n;
  struct proc *p;
  struct mlfq *mlfq = &mlfqueue;
  struct lwp_group *lwps;

  if(mlfq->totalticks >= 200){

    if(mlfq->mqueue->count > 0){
      for(n = mlfq->mqueue->front; n; n = n->next){
	lwps = n->proc->my_lwp_group;
	lwps->quantum = mlfq->hqueue->quantum;
	lwps->allotment = mlfq->hqueue->allotment;
	lwps->ticks = 0;
      }
    }

    if(mlfq->lqueue->count > 0){
      for(n = mlfq->lqueue->front; n; n = n->next){
        lwps = n->proc->my_lwp_group;
        lwps->quantum = mlfq->hqueue->quantum;
        lwps->allotment = mlfq->hqueue->allotment;
        lwps->ticks = 0;
      }
    }

    for(n = mlfq->hqueue->front; n; n = n->next){
      p = n->proc;
      p->tick = 0;
    }
    for(n = mlfq->mqueue->front; n; n = n->next){
      p = n->proc;
      struct node *temp = (struct node *)pop(mlfq->mqueue, p);
      push(mlfq->hqueue, temp);
      if(mlfq->mqueue->cur == n)
	adjustCurForRoundRobin(mlfq->mqueue, n);
      p->tick = 0;
      p->mlfq_level = 0;
    }
    for(n = mlfq->lqueue->front; n; n = n->next){
      p = n->proc;
      struct node *temp = (struct node *)pop(mlfq->lqueue, p);
      push(mlfq->hqueue, temp);
      if(mlfq->lqueue->cur == n)
        adjustCurForRoundRobin(mlfq->lqueue, n);

      p->tick = 0;
      p->mlfq_level = 0;
    }
    mlfq->totalticks = 0;

    return 1;
  }
  return 0;
}

void
stride_scheduling(void)
{
  struct priority_queue *pq = &priqueue;
  int pq_num = pq->count;

  struct proc *temp_proc[pq_num];
  int idx = 0;
  struct proc *p;  
  struct cpu *c = mycpu();
  struct lwp_group *lwps;
  struct lwp *lwp;
  struct proc *master;

  for(;;){
  idx = 0;
    while(1){
      p = pq_pop(pq);

      if(p->ismlfq){
	p->pass += p->stride;
	for(int i = 0; i < idx; i++)
	  pq_push(pq, temp_proc[i]);
	pq_push(pq, p);
	return;
      }

      lwps = p->my_lwp_group;
      if((lwp = findNextLwpToRun(lwps)) == 0){
	temp_proc[idx++] = p;
        continue;
      }
      master = p;
      break;
    }

    for(int i = 0; i < idx; i++)
      pq_push(pq, temp_proc[i]);

    p = lwp->proc;
    lwps->cur = lwp;
    c->proc = p;

    lwps->ticks++;
    master->pass += master->stride;
    lwps->pass += lwps->stride;
    pq_push(pq, master);

    switchuvm(p);
    p->state = RUNNING;
    swtch(&(c->scheduler), p->context);
    switchkvm();

    c->proc = 0; 
  }
}

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void
scheduler(void)
{
  struct node *n;
  struct proc *p;
  struct cpu *c = mycpu();
  c->proc = 0;

  //pointer to the global variables.
  struct mlfq *mlfq = &mlfqueue;
  struct proc *mproc = &mlfq_proc;
  struct priority_queue *pq = &priqueue;
 
  struct queue *hqueue = &high_queue;
  struct queue *mqueue = &mid_queue;
  struct queue *lqueue = &low_queue;

  struct lwp_group *lwps;
  struct lwp *lwp;

  //initialize priority queue(for stride scheduling)
  init_priqueue(pq);

  //initialize and push mlfq process(not a real process).
  init_mproc(mproc);
  pq_push(pq, mproc);

  for(;;){
    sti();
    
    acquire(&ptable.lock);

    //High priority q
    int all_unrunnable = 1;
    int unrunnable_count = 0;

    //cprintf("hqueue count : %d\n", hqueue->count);
   
    for(n = hqueue->front; n; n = n->next){
      p = n->proc;
      lwps = p->my_lwp_group;
      
      //find runnable lwp
      if((lwp = findNextLwpToRun(lwps)) == 0)
	continue;

      all_unrunnable = 0;

      p = lwp->proc;
      lwps->cur = lwp;    
      c->proc = p;

      mlfq->totalticks++;
      lwps->ticks++;

      if(chkboost() == 0)
	chkallot(hqueue, mqueue);

      switchuvm(p);
      p->state = RUNNING;
      swtch(&(c->scheduler), p->context);
      switchkvm();

      c->proc = 0;
      
      //stride scheduling
      if(pq->count > 1){
        stride_scheduling();
      }
    }
 
    //determine weather go down or not
    if((all_unrunnable == 0) && (hqueue->count > 0)){
      release(&ptable.lock);
      continue;
    }

    //Middle priority q
    all_unrunnable = 1;
    unrunnable_count = 0;

    for(n = mqueue->cur; ; ){

      if(unrunnable_count > mqueue->count || mqueue->count == 0)
	break;

      n = n->next;
      if(n == 0) 
	n = mqueue->front;

      p = n->proc;
      lwps = p->my_lwp_group;

      if((lwp = findNextLwpToRun(lwps)) == 0){
	unrunnable_count++;
	continue;
      }

      all_unrunnable = 0;

      p = lwp->proc;
      lwps->cur = lwp;
      mqueue->cur = n;
      c->proc = p;

      mlfq->totalticks++;
      lwps->ticks++;

      if(chkboost() == 0)
        chkallot(mqueue, lqueue);

      switchuvm(p);
      p->state = RUNNING;
      swtch(&(c->scheduler), p->context);
      switchkvm();

      c->proc = 0;
       
      if(pq->count > 1){
        stride_scheduling();
      }
      break;
    }
    
    //determine weather go down or not
    if((all_unrunnable == 0) && (mqueue->count > 0)){
      release(&ptable.lock);
      continue;
    }

    //Low priority q
    all_unrunnable = 1;
    unrunnable_count = 0;

    for(n = lqueue->cur; ;){

      if(unrunnable_count > lqueue->count || lqueue->count == 0)
	break;

      n = n->next;
      if(n == 0)
        n = lqueue->front;

      p = n->proc;
      lwps = p->my_lwp_group;
      if((lwp = findNextLwpToRun(lwps)) == 0){
	unrunnable_count++;
	continue;
      }

      all_unrunnable = 0;

      p = lwp->proc;
      lwps->cur = lwp;
      lqueue->cur = n;
      c->proc = p;

      mlfq->totalticks++;
      lwps->ticks++;

      chkboost();

      switchuvm(p);
      p->state = RUNNING;
      swtch(&(c->scheduler), p->context);
      switchkvm();

      c->proc = 0;

      //stride scheduling
      if(pq->count > 1){
        stride_scheduling(); 
      }     
      break;
    } 

    if(pq->count > 1){
      stride_scheduling();
    }
    release(&ptable.lock);
  }
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct cpu *c = mycpu();
  struct proc *curproc = myproc();
  struct proc *mproc = &mlfq_proc;
  struct mlfq *mlfq = &mlfqueue;
  struct priority_queue *pq = &priqueue;
  struct lwp_group *lwps = curproc->my_lwp_group;
  struct lwp *lwp;

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(mycpu()->ncli != 1)
    panic("sched locks");
  if(curproc->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");

  intena = mycpu()->intena;  

  if(lwps->ticks == 0 || (lwps->ticks % lwps->quantum) != 0){
    if((lwp = findNextLwpToRun(lwps)) == 0)
      swtch(&(curproc->context), mycpu()->scheduler);
    else{
      lwps->cur = lwp;
      c->proc = lwp->proc;
      lwp->proc->state = RUNNING;
      lwps->ticks++;

      //come from mlfq.
      if(lwps->ticket == 0){
	 mlfq->totalticks++;
        if(lwps->ticks > 5 && lwps->ticks % 5 == 0){
	  pq_select_pop(pq, mproc->pid);
	  mproc->pass += mproc->stride;
	  pq_push(pq, mproc);
	}
      }

      if(curproc != lwp->proc){
	switchuvm(lwp->proc);
        swtch(&(curproc->context), lwp->proc->context);
      }
    }
  }
  else{
    swtch(&(curproc->context), mycpu()->scheduler);
  }
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  myproc()->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  //cprintf("thread comes forkret: %d\n", myproc()->pid);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  if(p == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }
  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;
  
  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan)
      p->state = RUNNABLE;
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;
  struct lwp_group *lwps;
  struct lwp *lwp;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      lwps = p->my_lwp_group;
      for(lwp = lwps->front; lwp; lwp = lwp->next){
        lwp->proc->killed = 1;
        // Wake process from sleep if necessary.
        if(lwp->proc->state == SLEEPING)
          lwp->proc->state = RUNNABLE;
      }
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  int i;
  struct proc *p;
  char *state;
  uint pc[10];

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}

int
getppid(void)
{
    return myproc()->parent->pid;
}

//From here, LWP starts.
int
thread_create(thread_t *thread, void *(*start_routine)(void *), void *arg)
{
  struct proc *nt;
  struct proc *curproc = myproc();

  uint sz, sp, ustack[3];
  pde_t *pgdir;
  uint a, newsz, oldsz;

  struct lwp_group *lwps = curproc->my_lwp_group;
  struct lwp *lwp;

  if((nt = allocproc()) == 0){
    cprintf("allocproc panic\n");
    return -1;
  }

  *thread = nt->pid;

  nt->parent = curproc->parent;
  *nt->tf = *curproc->tf;
  nt->tf->eip = (uint)(start_routine);
  for(int fd = 0; fd < NOFILE; fd ++){
    nt->ofile[fd] = curproc->ofile[fd];
  } 
  nt->cwd = curproc->cwd;
  safestrcpy(nt->name, curproc->name, sizeof(curproc->name));

  //allocate user stack.
  sz = PGROUNDUP(lwps->sz);
  pgdir = curproc->pgdir;
  oldsz = sz;
  newsz = sz + 2*PGSIZE;

  if(newsz >= KERNBASE){
    uint start = PGROUNDUP(KERNBASE-PGSIZE);
    int success = 0;
    pte_t *pte1;
    pte_t *pte2;
    for(uint i = start; i > curproc->static_size + 2*PGSIZE; i -= PGSIZE){
      pte1 = walkpgdir(pgdir, (char*)(i-PGSIZE), 0);
      pte2 = walkpgdir(pgdir, (char*)(i-2*PGSIZE), 0);
      if(!(*pte1 & PTE_P) && !(*pte2 & PTE_P)){
	cprintf("kernbase overflow...found..\n");
	allocuvm(pgdir, i-2*PGSIZE, i);
	sz = oldsz;
	nt->ustack_va = i;
	sp = i;
	clearpteu(pgdir, (char*)(i-2*PGSIZE));
	success = 1;
	break;
      }
    }
    if(!success){
      cprintf("kernbase panic\n");
      return -1;
    }
  }
  else{
    a = PGROUNDUP(oldsz);
    sz = allocuvm(pgdir, a, newsz);
    nt->ustack_va = sz;
    clearpteu(pgdir, (char*)(sz - 2*PGSIZE));
    sp = sz;
  }

  // Push argument
  ustack[0] = nt->tf->eip;
  ustack[1] = (int)arg;

  sp -= 2*4;
  if(copyout(pgdir, sp, ustack, 2*4) < 0){
    freevm(pgdir);
    cprintf("copyout panic\n");
    return -1;
  }

  acquire(&ptable.lock);

  curproc->pgdir = pgdir;
  nt->pgdir = pgdir;
  nt->tf->esp = sp;
 
  //for scheduling
  nt->state = RUNNABLE;
  nt->mlfq_level = 0;
  nt->ismlfq = 0;

  //for lwp management
  createLwpNode(nt, &lwp_node[nt->ptable_idx]);
  appendLwpNode(lwps, &lwp_node[nt->ptable_idx]);
  nt->my_lwp_group = lwps;

  lwps->sz = sz;
  for(lwp = lwps->front; lwp; lwp = lwp->next)
    lwp->proc->sz = sz;

  nt->ismaster = 0;

  release(&ptable.lock);

  return 0;
}

void
thread_exit(void *retval)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;
  struct lwp_group *lwps = curproc->my_lwp_group;

  //cprintf("thread_exit\n");

  if(curproc == initproc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
      if(lwps->count == 1)
        fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }
  if(lwps->count == 1){
    begin_op();
    iput(curproc->cwd);
    end_op();
  }
  curproc->cwd = 0;

  acquire(&ptable.lock);
  curproc->thread_retval = retval;

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
  wakeup1(curproc);
  sched();
  panic("zombie exit");	
}

//if deallocated stack is at the top of the user space, 
//update the sz variable.
void
adjustUvmSize(struct proc *p)
{
  struct lwp_group *lwps = p->my_lwp_group;
  struct lwp *lwp;
  uint newsz = 0;

  if(p->ustack_va == p->sz){
    newsz = PGROUNDUP(p->sz);
    pte_t *pte;
    for(; newsz > p->static_size; newsz -= PGSIZE){
      pte = walkpgdir(p->pgdir, (char *)newsz, 0);
      if(*pte & PTE_P) break;
    }
    newsz += PGSIZE;
    lwps->sz = newsz;
    for(lwp = lwps->front; lwp; lwp = lwp->next)
      lwp->proc->sz = newsz;
  }
}

int
thread_join(thread_t thread, void **retval)
{
  struct proc *p;
  struct proc *channel = 0;
  struct proc *curproc = myproc();
  struct mlfq *mlfq = &mlfqueue;
  struct lwp_group *lwps;
  struct priority_queue *pq = &priqueue;

  //cprintf("thread_join\n");

  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->pid != thread)
        continue;
      channel = p;
      lwps = p->my_lwp_group;
      if(p->state == ZOMBIE){
        // Found one.

	if(p->ismaster){
          popFromSchedulingQueue(p);	
	  if(lwps->count > 1){
	    struct lwp *newMaster = selectOtherMaster(p);
	    if(newMaster->proc->ticket == 0)
              pushToMlfq(mlfq, &node[newMaster->proc->ptable_idx], 
	      newMaster->proc); 
	    else
	      pq_push(pq, newMaster->proc); 
	  }
	}

	deallocuvm(p->pgdir, p->ustack_va, p->ustack_va-2*PGSIZE);
	adjustUvmSize(p);
	p->ustack_va = 0;

	//pop from lwp group.
	removeLwpNode(lwps, p->pid);
	p->my_lwp_group = 0;

        kfree(p->kstack);
        p->kstack = 0;
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
	p->pgdir = 0;

	*retval = p->thread_retval;
	p->thread_retval = 0;

        p->state = UNUSED;
        release(&ptable.lock);
	return 0;
      }
    }

    // No point waiting if we don't have any children.
    if(curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(channel, &ptable.lock);  //DOC: wait-sleep
  }
}
