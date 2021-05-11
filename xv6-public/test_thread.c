#include "types.h"
#include "stat.h"
#include "user.h"

#define THREAD_NUMBER 10

int THREADMAX = 10000;
int global_counter;

void *counting(void *arg)
{
  int local_times = *(int*)arg;
  int local_counter = 0;
  int i;

  for (i = 0; i < local_times; i++){
    global_counter++;
    local_counter++;
  }

  thread_exit((void*)local_counter);
  return 0;
}

int main(int argc, char *argv[])
{
  int i;
  thread_t pid[THREAD_NUMBER];
  int ret_vals[THREAD_NUMBER];
  int thread_args[THREAD_NUMBER];

  for(i = 0; i < THREAD_NUMBER; i++){
    thread_args[i] = THREADMAX * (i + 1);
    thread_create(&pid[i], counting, (void*)&thread_args[i]);
  }

  for(i = 0; i < THREAD_NUMBER; i++){
    thread_join(pid[i], (void*)&ret_vals[i]);
  }

  for(i = 0; i < THREAD_NUMBER; i++){
    printf(1, "Local Counter: %d\n", ret_vals[i]);
  }

  printf(1, "Global Counter: %d\n", global_counter);

  exit();
}
