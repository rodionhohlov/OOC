
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <math.h>

//sin^2(x)
void *squared_sin (void *arg)
{
    double x = *(double *)arg;

    double *result = malloc(sizeof(double));
    *result = sin(x) * sin(x);

    return result;
}

//cos^2(x)
void *squared_cos (void *arg)
{
    double x = *(double *)arg;

    double *result = malloc(sizeof(double));
    *result = cos(x) * cos(x);

    return result;
}

int main (void)
{
    pthread_t thread1, thread2;

    double x = 1.0;
    int N = 100;
    
    for (int i = 0; i < N; i++) {

        void *result1;
        void *result2;

        pthread_create(&thread1, NULL, squared_sin, &x);
        pthread_create(&thread2, NULL, squared_cos, &x);

        pthread_join(thread1, &result1);
        pthread_join(thread2, &result2);

        double sin2 = *(double *)result1;
        double cos2 = *(double *)result2;

        double sum = sin2 + cos2;

        printf("------------------------\n");
        printf("iteration: %d\n", i);
        printf("sin²(x) = %.10f\n", sin2);
        printf("cos²(x) = %.10f\n", cos2);
        printf("sum = %.10f\n", sum);

        free(result1);
        free(result2);

    }

    return 0;
}
