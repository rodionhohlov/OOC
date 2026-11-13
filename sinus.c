
#include <stdio.h>
#include <pthread.h>
#include <math.h>
#include <time.h>

#define N 100
#define MAX_THREADS 10

double x = 1.0;
double results[MAX_THREADS];

typedef struct {
    int start;
    int end;
    int id;
} ThreadData;

double talor_member (int n)
{
    double result = x;

    for (int i = 1; i <= n; i++)
    {
        result *= -x * x / ((2.0 * i) * (2.0 * i + 1));
    }

    return result;
}

// Функция потока
void *thread_func (void *arg)
{
    ThreadData *data = (ThreadData *)arg;

    double sum = 0;

    for (int i = data->start; i < data->end; i++)
    {
        sum += term(i);
    }

    results[data->id] = sum;

    return NULL;
}

int main(void)
{
    pthread_t threads[MAX_THREADS];
    ThreadData data[MAX_THREADS];

    struct timespec start, end;

    printf("Standard sin(x) = %.15f\n\n", sin(x));

    // Запускаем от 1 до 10 потоков
    for (int count = 1; count <= MAX_THREADS; count++)
    {
        clock_gettime(CLOCK_MONOTONIC, &start);

        // Создаем потоки
        for (int i = 0; i < count; i++)
        {
            data[i].id = i;
            data[i].start = i * N / count;
            data[i].end = (i + 1) * N / count;

            if (pthread_create(&threads[i], NULL,
                               thread_func, &data[i]) != 0)
            {
                fprintf(stderr, "Thread error\n");
                return 1;
            }
        }

        // Ждем завершения потоков
        for (int i = 0; i < count; i++)
        {
            pthread_join(threads[i], NULL);
        }

        // Складываем результаты потоков
        double sum = 0;

        for (int i = 0; i < count; i++)
        {
            sum += results[i];
        }

        clock_gettime(CLOCK_MONOTONIC, &end);

        double elapsed =
            (end.tv_sec - start.tv_sec) +
            (end.tv_nsec - start.tv_nsec) / 1e9;

        double error = fabs(sum - sin(x));

        printf("Threads: %2d | sin(x): %.15f | Error: %.3e | Time: %.6f ms\n", count, sum, error, elapsed * 1000);
    }

    return 0;
}
