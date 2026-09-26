/* Создать программу, запускаемую на N терминалах, для обмена сообщениями между терминалами через файл.
 Каждый экземпляр программы, запущенный на каждом терминале содержит два процесса:
 один записывает файл, второй читает. Так на двух терминалах работает четыре процесса,
 обеспечивая двусторонний обмен. 
 Идентификатор пользователя (терминала), можно задавать параметром при запуске приложения.
 Предусмотреть хранение информации для отсутствующего получателя.
 Начать разработку со схемы данных. Предусмотреть поля для идентификаторов, отправителя и получателя,
а так же данных.
Разработать протокол обмена и структуру файла.
Продумать протокол очистки файла.
Исследовать вопрос с уникальностью идентификаторов.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/wait.h>

#define USER_ID_LEN 32
#define MESSAGE_LEN 256
#define CONTEXT_LEN 512
#define FILENAME    "messages.dat"

enum spec_values_t {
    UNREAD = 0,
    READ   = 1
};

typedef struct {
    char   sender_id[USER_ID_LEN];
    char   receiver_id[USER_ID_LEN];
    char   message_text[MESSAGE_LEN];
    int    is_read_flag;
    time_t send_time;
    long   message_id;
} message_context;

static volatile sig_atomic_t stop_flag = 0;

// ----------------------------funcs-----------------------------

void WriterProcess(const char *my_id, const char *filename);

void ReaderProcess(const char *my_id, const char *filename);

void OnSignal (int sig);

// ---------------------------- main ---------------------------- 
int main(int argc, char *argv[]) {

    if (argc < 2) {
        fprintf(stderr, "Использование: %s <ID_терминала>\n", argv[0]);
        return EXIT_FAILURE;
    }

    if (strlen(argv[1]) >= USER_ID_LEN) {
        fprintf(stderr, "ID слишком длинный (max %d)\n", USER_ID_LEN - 1);
        return EXIT_FAILURE;
    }

    const char *my_id    = argv[1];
    const char *filename = FILENAME;

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return EXIT_FAILURE;
    }

    if (pid == 0) {
    
        ReaderProcess(my_id, filename);
        _exit(EXIT_SUCCESS);
    }

    WriterProcess(my_id, filename);

    kill(pid, SIGTERM); //уничножение созданного ранее процесса

    if (waitpid(pid, NULL, 0) < 0) {
        perror("waitpid");
    }

    printf("Завершение работы.\n");
    return EXIT_SUCCESS;
}

void OnSignal (int sig) {
    (void)sig;
    stop_flag = 1;
}

void WriterProcess (const char *my_id, const char *filename) {

    if(my_id == NULL || filename == NULL) {
        fprintf(stderr, "Ошибка передечи аргуменов в writer_process\n");
        return;
    }
    
    int fd = open(filename, O_RDWR | O_CREAT, 0666);

    if (fd < 0) {
        perror("ошибка создания файла");
        exit(EXIT_FAILURE);
    }

    printf("Writer (ID %s). Вводите: <ID_получателя> <текст>\n", my_id);
    printf("Для выхода нажмите Ctrl+D.\n");

    char line[CONTEXT_LEN];
    ssize_t n = 0;

    while ((n = read(STDIN_FILENO, line, sizeof(line) - 1)) > 0) {
        line[n] = '\0';

        char *nl = strchr(line, '\n');

        if (nl != NULL) {
            *nl = '\0';
        }

        if (line[0] == '\0') {
            continue;
        }

        char recipient[USER_ID_LEN] = {0};
        char text[MESSAGE_LEN]      = {0};

        if (sscanf(line, "%31s %255[^\n]", recipient, text) != 2) {
            printf("Формат: <ID_получателя> <текст>\n");
            continue;
        }

        message_context msg = {};

        time_t now = time (NULL);

        msg.message_id = (long) now * 100000 + (getpid() % 100000);
        strncpy(msg.sender_id,   my_id,     USER_ID_LEN - 1);
        strncpy(msg.receiver_id, recipient, USER_ID_LEN - 1);
        strncpy(msg.message_text, text,     MESSAGE_LEN - 1);
        msg.send_time    = now;
        msg.is_read_flag = UNREAD;

        if (flock(fd, LOCK_EX) < 0) {
            perror("ошибка flock"); 
        }

        if (lseek(fd, 0, SEEK_END) < 0) {
            perror("ошибка lseek");
        }

        if (write(fd, &msg, sizeof(msg)) != (ssize_t)sizeof(msg)) {
            perror("ошибка write");
        }

        if (flock(fd, LOCK_UN) < 0) {
            perror("ошибка flock");
            close(fd);
            exit(EXIT_FAILURE);
        }

        printf("Отправлено %s: %s\n", recipient, text);
    }

    if (close(fd) < 0) {
        perror("ошибка закрытия файла");
    }
}

void ReaderProcess (const char *my_id, const char *filename) {

    if (my_id == NULL || filename == NULL) {
        fprintf(stderr, "Ошибка передечи аргуменов в reader_process\n");
        return;
    }

    signal(SIGTERM, OnSignal);
    signal(SIGINT,  OnSignal);

    int fd = open(filename, O_RDWR | O_CREAT, 0666);
    if (fd < 0) {
        perror("open");
        exit(EXIT_FAILURE);
    }

    while (!stop_flag) {
        if (flock(fd, LOCK_EX) < 0) { 
            perror("ошибка flock"); 
            break;
        }

        message_context *arr = NULL;
        size_t count = 0, cap = 0;
        message_context msg;

        if (lseek(fd, 0, SEEK_SET) < 0) perror("lseek");

        ssize_t n;

        while ((n = read(fd, &msg, sizeof(msg))) == (ssize_t)sizeof(msg)) {
            if (count == cap) {

                if (cap == 0) {
                    cap = 16;
                } else {
                    cap *= 2;
                }

                message_context *tmp = realloc(arr, cap * sizeof(message_context));

                if (!tmp) { 
                    perror("ошибка realloc"); 
                    break; 
                }

                arr = tmp;
            }

            arr[count++] = msg;
        }

        int need_cleanup = 0;

        for (size_t i = 0; i < count; i++) {

            if (arr[i].is_read_flag == READ) {
                need_cleanup = 1;
            }

            if (strcmp(arr[i].receiver_id, my_id) == 0 && arr[i].is_read_flag == UNREAD) {

                char timebuf[64];
                struct tm *tm_info = localtime(&arr[i].send_time);

                strftime(timebuf, sizeof(timebuf), "%H:%M:%S", tm_info);

                printf("\n[%s] от %s: %s\n",
                       timebuf, arr[i].sender_id, arr[i].message_text);
                fflush(stdout);

                arr[i].is_read_flag = READ;
                need_cleanup = 1;
            }
        }

        if (need_cleanup) {
            if (lseek(fd, 0, SEEK_SET) < 0) {
                perror("lseek");
            }

            size_t new_count = 0;
            for (size_t i = 0; i < count; i++) {
                if (arr[i].is_read_flag == UNREAD) {
                    if (write(fd, &arr[i], sizeof(message_context))
                        != (ssize_t)sizeof(message_context)) {
                        perror("write");
                    }
                    new_count++;
                }
            }
            if (ftruncate(fd, (off_t)(new_count * sizeof(message_context))) < 0) {
                perror("ftruncate");
            }
        }

        free(arr);

        if (flock(fd, LOCK_UN) < 0) {
            perror("flock");
            close(fd);
            exit(EXIT_FAILURE);
        }

        for (int i = 0; i < 10 && !stop_flag; i++) {
            usleep(100000);
        }
    }

    if (close(fd) < 0) perror("close");
}