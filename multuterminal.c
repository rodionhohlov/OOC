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
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>

#define MESSAGE_LEN 256
#define ID_LEN 32

const char filename[] = "transit.txt";

typedef struct {
    char sender_id[ID_LEN];
    char reciever_id[ID_LEN];
    bool status;
    char message_text[MESSAGE_LEN];
    int message_id;
} message_context_t;

void init_file_if_need (void);

void clean_transit_file (void);

void reader_process(const char* my_id);

void writer_process(const char* my_id);

int lock_file(int fd, int type);

int main (int argc, char* argv[]) {

    if (argc < 2) {
        perror("user_id is undefined\n");
        exit(1);
    }

    char my_id[ID_LEN];
    snprintf(my_id, sizeof(my_id), "%s", argv[1]);

    init_file_if_need();
    clean_transit_file();

    pid_t process_id = fork();

    if (process_id < 0) {
        perror("process init error\n");
        exit(1);
    }
    else if (process_id == 0) {
        reader_process(my_id);
    }
    else {
        writer_process(my_id);
        wait(NULL);
    }
    
    return 0;
}

void init_file_if_need (void) {
    int file_id = open(filename, O_CREAT | O_RDWR, 0666);

    if (file_id < 0) {
        perror("file opening failed\n");
        exit(1);
    }

    close(file_id);
}

void clean_transit_file(void) {
    int fd = open(filename, O_RDWR | O_CREAT, 0666);

    if (fd < 0) {
        perror("opening file for cleaning failed\n");
        return;
    }

    lock_file(fd, F_WRLCK);

    message_context_t messages[1000];
    int count = 0;

    while (read(fd, &messages[count], sizeof(message_context_t)) == sizeof(message_context_t)) {
        if (messages[count].status == false) {
            count++;
        }
        if (count >= 1000) break;
    }

    ftruncate(fd, 0);
    lseek(fd, 0, SEEK_SET);

    for (int i = 0; i < count; i++) {
        write(fd, &messages[i], sizeof(message_context_t));
    }

    lock_file(fd, F_UNLCK);
    close(fd);
}

int lock_file(int fd, int type) {
    struct flock fl;
    fl.l_type = type;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;
    return fcntl(fd, F_SETLKW, &fl);
}

void reader_process(const char* my_id) {
    
    int fd = open(filename, O_RDWR | O_CREAT, 0666);

    if (fd < 0) {
        perror("file opening failed in mode reading\n");
        exit(1);
    }

    while (true) {
        lock_file(fd, F_WRLCK);

        lseek(fd, 0, SEEK_SET);

        message_context_t message = {};
        off_t offset = 0;

        while (read(fd, &message, sizeof(message_context_t)) == sizeof(message_context_t)) {

            if (strcmp(message.reciever_id, my_id) == 0 && message.status == false) {

                printf("\n[message from %s]: %s\n> ", message.sender_id, message.message_text);
                fflush(stdout);

                message.status = true;
                lseek(fd, offset, SEEK_SET);
                write(fd, &message, sizeof(message_context_t));
                
                lseek(fd, offset + sizeof(message_context_t), SEEK_SET);
            }

            offset = lseek(fd, 0, SEEK_CUR);
        }

        lock_file(fd, F_UNLCK);
        sleep(1);
    }

    close(fd);
}

void writer_process(const char* my_id) {
    int fd = open(filename, O_RDWR | O_CREAT, 0666);

    if (fd < 0) {
        perror("opening file error\n");
        exit(1);
    }

    char input_line[MESSAGE_LEN + ID_LEN + 32];
    char recipient[ID_LEN];
    char text[MESSAGE_LEN];

    printf("\nuser %s is ready.\n", my_id);
    printf("please, write down: <recipient> <message>\n> ");
    fflush(stdout);

    while (fgets(input_line, sizeof(input_line), stdin) != NULL) {
    
        input_line[strcspn(input_line, "\n")] = 0;

        if (strlen(input_line) == 0) {
            printf("> ");
            fflush(stdout);
            continue;
        }

        int parsed = sscanf(input_line, "%31s %[^\n]", recipient, text);
        
        if (parsed < 2) {
            printf("Error: format should be <recipient> <message>\n> ");
        } else {
            message_context_t msg = {0};

            snprintf(msg.sender_id, ID_LEN, "%s", my_id);
            snprintf(msg.reciever_id, ID_LEN, "%s", recipient);
            msg.status = false;
            msg.message_id = (int)(time(NULL) ^ getpid()); 
            snprintf(msg.message_text, MESSAGE_LEN, "%s", text);

            lock_file(fd, F_WRLCK);
            lseek(fd, 0, SEEK_END);
            write(fd, &msg, sizeof(message_context_t));
            lock_file(fd, F_UNLCK);
        }

        printf("> ");
        fflush(stdout);
    }

    close(fd);
}