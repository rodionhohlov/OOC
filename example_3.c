/* Пример создания нового процесса с разной работой процессов ребенка и родителя 
Исследовать как изменяться результаты, если продлить время жизни одного из процессов
при помощи задержки.
*/

#include <sys/types.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
 
int main() {
    
pid_t pid = 0, ppid = 0, chpid = 0;

int a = 0;
 
chpid = fork();

if (chpid < 0) {
  printf("Ошибка\n");
} 

else if (chpid == 0) {
  /* Порожденный процесс */
  a = a+1; 
  pid = getpid();
  ppid = getppid();

  printf("pid = %d, ppid = %d, a = %d\n", (int) pid, (int) ppid, a); 
}

else {
  /* Родительский процесс */
  pid = getpid();
  ppid = getppid();

  printf("My pid = %d, my ppid = %d, result = %d\n", (int) pid, (int) ppid, a); 
 
}

return 0;
}