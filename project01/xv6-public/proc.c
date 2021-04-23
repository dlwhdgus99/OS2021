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
int index;

struct proc mlfq_proc;
struct priority_queue priqueue;

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
      index = i;
      goto found;
    }

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;

  release(&ptable.lock);

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
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

void
init_qcur(struct queue *q)
{
  q->cur = q->front;
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

  //initialize 3 level queues.
  initqueue(hqueue, 1,5);
  initqueue(mqueue, 2,10);
  initqueue(lqueue, 4,0);

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
  createnode(p, &node[index]);
  push(hqueue, &node[index]);

  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();

  sz = curproc->sz;
  if(n > 0){
    if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  curproc->sz = sz;
  switchuvm(curproc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();

  struct mlfq *mlfq = &mlfqueue;

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy process state from proc.
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = curproc->sz;
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
   
  cprintf("pid %d fork\n", pid);

  np->state = RUNNABLE;
  np->tick = 0;
  np->mlfq_level = 0;
  np->ismlfq = 0;
  createnode(np, &node[index]);
  push(mlfq->hqueue, &node[index]);
  if(mlfq->hqueue->count == 1){
    mlfq->hqueue->cur = mlfq->hqueue->front;
  }

  release(&ptable.lock);

  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;

  if(curproc == initproc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

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
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();
  struct mlfq *mlfq = &mlfqueue;
  struct priority_queue *pq = &priqueue;
  struct proc *mproc = &mlfq_proc;

  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.

	if(p->ticket == 0){
	  if(p->mlfq_level == 0){
            pop(mlfq->hqueue, p);
          }
          else if(p->mlfq_level == 1){
            pop(mlfq->mqueue, p);
	    if(mlfq->mqueue->cur->next == 0){
	      mlfq->mqueue->cur = mlfq->mqueue->front;
	    }
	    else{
	      mlfq->mqueue->cur = mlfq->mqueue->cur->next;
	    }
          }
          else{
            pop(mlfq->lqueue, p);
	    if(mlfq->lqueue->cur->next == 0){
              mlfq->lqueue->cur = mlfq->lqueue->front;
            }
            else{
              mlfq->lqueue->cur = mlfq->lqueue->cur->next;
            }
          }
	}
	else{
	  pq_select_pop(pq, p->pid);
	  mproc->ticket += p->ticket;
	}
//        cprintf("\n");
//	cprintf("-----------state report------------------\n");
//	cprintf("After pid %d exit\n", p->pid);
//	cprintf("MLFQ\n");
 //	cprintf("hqueue count: %d\n", mlfq->hqueue->count);
//	cprintf("mqueue count: %d\n", mlfq->mqueue->count);
//	cprintf("lqueue count: %d\n", mlfq->lqueue->count);
//	cprintf("STRIDE\n");
//	cprintf("priority queue count: %d\n", pq->count);
//	cprintf("\n");


        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
      
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
  struct proc *curproc = myproc();
  return curproc->mlfq_level;
}

int
set_cpu_share(int share)
{
  struct proc *curproc = myproc();
  struct mlfq *mlfq = &mlfqueue;
  struct proc *mproc = &mlfq_proc;
  struct priority_queue *pq = &priqueue;
 
  if(mproc->ticket - share < 20){
    panic("greedy");
  }

  acquire(&ptable.lock);
  if(curproc->mlfq_level == 0){
    pop(mlfq->hqueue, curproc);
  }
  else if(curproc->mlfq_level == 1){
    pop(mlfq->mqueue, curproc);
  }
  else{
    pop(mlfq->lqueue, curproc);
  }
  mproc->ticket -= share;
  mproc->stride = LNUM/mproc->ticket;

  curproc->ticket = share;
  curproc->stride = LNUM/curproc->ticket;
  curproc->pass = pq->heap[0]->pass;
  curproc->ismlfq = 0;

  pq_push(pq, curproc); 

  release(&ptable.lock);
  return 0;
}

int
chkquant(struct queue *q)
{
  struct proc *p = myproc();

  if((p->tick != 0) && ((p->tick) % q->quantum == 0)){
    if(q->cur->next == 0){
      q->cur = q->front;
    }
    else{
      q->cur = q->cur->next;
    }
   
    return 1;
  }
  return 0;
}

int
chkallot(struct queue *now,  struct queue *low)
{
  struct proc *p = myproc();  
  struct node *candidate_cur = now->cur->next;
 
  if(p->tick == now->allotment){
    struct node *temp = (struct node *)pop(now, p);
    if(now->cur == temp){
      if(candidate_cur == 0){
	now->cur = now->front;
      }
      else{
	now->cur = candidate_cur;
      }
    }
    if(temp == 0){
      cprintf("chkallot!!\n");
    }
    push(low, temp);
    if(low->count == 1){
      init_qcur(low);
    }
    p->tick = 0;
    p->mlfq_level++;
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

  if(mlfq->totalticks == 100){
    for(n = mlfq->hqueue->front; n != 0; n = n->next){
      p = n->proc;
      p->tick = 0;
    }
    for(n = mlfq->mqueue->front; n != 0; n = n->next){
      p = n->proc;
      struct node *temp = (struct node *)pop(mlfq->mqueue, p);
      if(temp == 0){
	cprintf("chkbooost: mqueue\n");
      }
      push(mlfq->hqueue, temp);
      p->tick = 0;
      p->mlfq_level = 0;
    }
    for(n = mlfq->lqueue->front; n != 0; n = n->next){
      p = n->proc;
      struct node *temp = (struct node *)pop(mlfq->lqueue, p);
      if(temp == 0){
	cprintf("chkbooost: lqueue\n");
      }
      push(mlfq->hqueue, temp);
      p->tick = 0;
      p->mlfq_level = 0;
    }
    mlfq->totalticks = 0;
    return 1;
  }
  return 0;
}

void
init_mproc(void)
{
  struct proc *mproc = &mlfq_proc;
  
  mproc->state = RUNNABLE;
  mproc->pid = -1;
  mproc->tick = -1;
  mproc->pass = 0;
  mproc->ticket = 100;
  mproc->stride = LNUM/mproc->ticket;
  mproc->ismlfq = 1;
  mproc->mlfq_level = -1;
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

  for(;;){
    p = pq_pop(pq);
    while(p->state != RUNNABLE){
      temp_proc[idx++] = p;
      p = pq_pop(pq);
    }
    for(int i = 0; i < idx; i++){
      pq_push(pq, temp_proc[i]);
    }
    
//    p = pq_pop(pq);   

    if(p->ismlfq == 1){
      p->pass += p->stride;
      pq_push(pq, p);
      return ;
    }

    c->proc = p;
    switchuvm(p);
    p->state = RUNNING;
    swtch(&(c->scheduler), p->context);
    switchkvm();
    c->proc = 0; 
    if(p->pid != 0){
      p->pass += p->stride;
      pq_push(pq, p);
    }
  }
}

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

  //initialize priority queue(for stride scheduling)
  init_priqueue(pq);

  //initialize current process in the highest level queue.
  init_qcur(hqueue);
  
  //initialize and push mlfq process(not a real process).
  init_mproc();
  pq_push(pq, mproc);

  for(;;){
    sti();
    
    acquire(&ptable.lock);

    //High priority q
    int allunrunnable = 1;

    //cprintf("hqueue count : %d\n", hqueue->count);
   
    for(n = hqueue->front; n != 0; n = n->next){
      p = n->proc;
      if(p->state != RUNNABLE){
	continue;
      }
      allunrunnable = 0;

      hqueue->cur = n;
      c->proc = p;
      switchuvm(p);
      p->state = RUNNING;
      
      swtch(&(c->scheduler), p->context);
      switchkvm();

      mlfq->totalticks++;
      if(p->pid != 0){
	p->tick++;
	if(chkboost() == 0){
	  chkallot(hqueue, mqueue);
	}
      }
      chkboost();

      c->proc = 0;
      
      //stride scheduling
      if(pq->count > 1){
        stride_scheduling();
      }
    }
 
    //determine weather go down or not
    if((allunrunnable == 0) && (hqueue->count > 0)){
      release(&ptable.lock);
      continue;
    }

    //cprintf("mqueue count : %d\n", mqueue->count);

    //Middle priority q
    allunrunnable = 1;
    for(n = mqueue->cur; n != 0; n = n->next){
      p = n->proc;
      if(p->state != RUNNABLE){
	continue;
      }
      allunrunnable = 0;
      
      mqueue->cur = n;
      c->proc = p;    
  
      switchuvm(p);
      p->state = RUNNING;
      swtch(&(c->scheduler), p->context);
      switchkvm();

      mlfq->totalticks++;
      if(p->pid != 0){
        p->tick++;
        if(chkboost() == 0){
          if(chkallot(mqueue, lqueue) == 0){
	    chkquant(mqueue);
	  }
        }
      }
      chkboost();

      c->proc = 0;
       
      if(pq->count > 1){
        stride_scheduling();
      }
      break;
    }
    
    //determine weather go down or not
    if((allunrunnable == 0) && (mqueue->count > 0)){
      release(&ptable.lock);
      continue;
    }

    //Low priority q
    allunrunnable = 1;

    for(n = lqueue->cur; n != 0; n = n->next){
      p = n->proc;
      if(p->state != RUNNABLE){
	continue;
      }
      allunrunnable = 0;

      lqueue->cur = n;
      c->proc = p;

      switchuvm(p);
      p->state = RUNNING;
      swtch(&(c->scheduler), p->context);
      switchkvm();

      mlfq->totalticks++;
      if(p->pid != 0){
	p->tick++;
	if(chkboost() == 0){
	  chkquant(lqueue);
	}
      }
      chkboost();
      c->proc = 0;
 
      //stride scheduling
      if(pq->count > 1){
        stride_scheduling(); 
      }     
      break;
    }
    //determine weather go down or not
    if((allunrunnable == 0) && (lqueue->count > 0)){
      release(&ptable.lock);
      continue;
    }
    if(pq->count > 1){
      stride_scheduling();
    }
    release(&ptable.lock);
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
//void
//scheduler(void)
//{
//  myscheduler();
//}
//  struct proc *p;
//  struct cpu *c = mycpu();
//  c->proc = 0;
  
//  for(;;){
    // Enable interrupts on this processor.
//    sti();

//    // Loop over process table looking for process to run.
//    acquire(&ptable.lock);
//    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
//      if(p->state != RUNNABLE)
//        continue;

//      // Switch to chosen process.  It is the process's job
//      // to release ptable.lock and then reacquire it
//      // before jumping back to us.
//      c->proc = p;
//      switchuvm(p);
//      p->state = RUNNING;

//      swtch(&(c->scheduler), p->context);
//      switchkvm();

//      // Process is done running for now.
//      // It should have changed its p->state before coming back.
//      c->proc = 0;
//    }
//    release(&ptable.lock);

//  }
//}

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
  struct proc *p = myproc();

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(mycpu()->ncli != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&p->context, mycpu()->scheduler);
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
  
  //cprintf("\n---------------------\n");
  //cprintf("pid %d goes to sleep\n", p->pid);
  //cprintf("---------------------\n");

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

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING)
        p->state = RUNNABLE;
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

