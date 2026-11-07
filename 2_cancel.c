#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
/* Модифицировать программу, 
 * чтобы замерить среднее время завершения нити после сигнала на завершение.
 */
void * any_func (void * arg)
{
  while (1) 
  {
  	fprintf (stderr, ".");
  	sleep (10);
  }
  
  return NULL;
}

int main (void)
{
  int N = 100; //число замеров
  double total_time = 0;

  struct timespec start, end; 

  pthread_t thread;
  void * result;

  for (int i = 0; i < N; i++) {

    if (pthread_create (&thread, NULL, &any_func, NULL) != 0) 
    {
    	fprintf (stderr, "Error\n");
    	return 1;
    }

    usleep(10000);

    clock_gettime(CLOCK_MONOTONIC, &start);

    pthread_cancel (thread);

    if (!pthread_equal (pthread_self (), thread))
    {
      pthread_join (thread, &result);
    }

    clock_gettime(CLOCK_MONOTONIC, &end);

    double delta = ((end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9) * 1000;

    if (result == PTHREAD_CANCELED)
    {
      fprintf (stderr, "Thread cancelled: cancelling time %f ms\n", delta);
    }

    total_time += delta;
  }

  double time_res = total_time / 100.0;

  printf("-----------Results-----------\n"
         "      mean time: %f ms \n"
         "-----------------------------\n", time_res);

  return 0;
}