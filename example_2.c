/* Пример программы печатающей значения PPID и PID для текущего процесса */

#include <sys/types.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
 
int main() {

pid_t pid = 0, ppid = 0;
 
pid = getpid();
ppid = getppid();

if (pid == 0 || ppid == 0) {
    printf("incorrect pid\n");
    return 1;
}
 
printf("My pid = %d, my ppid = %d\n", pid, ppid);

return 0;
}