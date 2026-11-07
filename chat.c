#define _DEFAULT_SOURCE
/* Эта программа позволяет обмениваться сообщениями между терминалами.
  Произвести изменения. 
  1 расширить работу на N терминалов, добавив имя пользователя в параметры программы
  и имя получателя при вводе строки.
  2 Отметить какие моменты сильно влияют на надёжность работы программы*/

/*Надежность программы страдает при параллельной записи в память с нескольких терминалов, при чтении один изпроцессов читает быстрее всех и перекрывает дотуп к памяти другим зануляя флаг свободной памяти. Из за того что данные читаются постоянно они могут читаться во время попытки перезаписи */

#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <termios.h>
#include <sys/select.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/sem.h>

//#define DEBUG

// ---------- параметры ----------
#define USERNAME_MAX 32
#define RECIPIENT_MAX 32
#define TEXT_MAX 512
#define MAX_SLOTS 1024 //количество слотов сообщений
#define BROADCAST "*" //спецсимвол трансляции для всех

#define MAX_USERS 32
#define SEM_KEY_BASE 0x1BADB002U
#define CHAT_MAGIC 0x43544832U
#define SHM_KEY_BASE 0xDEADBABEU

enum { 
	DELAY_US = 10 * 1000, //  10 мс 
	PARCER_MISTAKE = -1,
	PARCER_SUCCESS = 0
};  

// ---------- структуры ---------- 

// Сообщение в разделяемой памяти
struct Message {
    int in_use;
    int checked_by_bot;

    char sender[USERNAME_MAX];
    char recipient[RECIPIENT_MAX];
    char message_text[TEXT_MAX];
};

// Информация о подключённом пользователе
struct User {
    char username[USERNAME_MAX];
    pid_t pid;
    int active;
};

// Общая память чата
// ВАЖНО: расположение полей совпадает с отдельной программой bot.c
struct SharedChat {
    uint32_t magic;
    struct Message slots[MAX_SLOTS];
    struct User users[MAX_USERS];
    int bot_active;
    int bot_enabled;
    pid_t bot_pid;
    unsigned checked_messages;
    unsigned unavailable_messages;
};

// Буфер набираемого сообщения
struct InputBuffer {
    char text[TEXT_MAX + RECIPIENT_MAX + 8];
    size_t len;
};

// Сообщение, передаваемое через pipe()
struct IncomingMessage {
    char sender[USERNAME_MAX];
    char recipient[RECIPIENT_MAX];
    char text[TEXT_MAX];
};

static const size_t SHMEM_SIZE = sizeof(struct SharedChat);

// ----------------- funcs --------------------

void HelloMessage (const char* username);

int  GetNum (const char* prompt);

int  ParseLine (char* line, char* recipient, char* text);

void PrintIncome (struct SharedChat *shm, int semid, const char* username, int pipe_fd);

void WriteToMem (struct SharedChat *shm, int semid, const char* username, int pipe_fd);

int EnableCharInput (struct termios *old_term);

int WaitForInput (int pipe_fd, fd_set *readfds);

int ShowIncoming(int pipe_fd, const struct InputBuffer *input, const char *username);

int SendMessage (struct SharedChat *shm, int semid, const char *username, const char *recipient, const char *text);

void SubmitInput (struct InputBuffer *input, struct SharedChat *shm, int semid, const char *username);

void EraseLastChar (struct InputBuffer *input);

int HandleKey (struct InputBuffer *input, struct SharedChat *shm, int semid, const char *username);

void RedrawInput (const struct InputBuffer *input, const char* username);

// ============== IPC: семафоры и пользователи ===============

// Захват и освобождение одного системного семафора
static int Lock(int semid)
{
    struct sembuf op = {0, -1, SEM_UNDO};
    int r;
    do { r = semop(semid, &op, 1); } while (r == -1 && errno == EINTR);
    return r;
}

static int Unlock(int semid)
{
    struct sembuf op = {0, 1, SEM_UNDO};
    int r;
    do { r = semop(semid, &op, 1); } while (r == -1 && errno == EINTR);
    return r;
}

// Создаём семафор один раз; другие терминалы только подключаются
static int OpenSemaphore(int room)
{
    key_t key = (key_t)((uint32_t)room + SEM_KEY_BASE);
    int semid = semget(key, 1, IPC_CREAT | IPC_EXCL | 0666);
    if (semid >= 0) {
        if (semctl(semid, 0, SETVAL, 1) == -1) return -1;
        // Отмечаем завершение инициализации через sem_otime
        struct sembuf ops[2] = {{0, -1, 0}, {0, 1, 0}};
        if (semop(semid, ops, 2) == -1) return -1;
        return semid;
    }
    if (errno != EEXIST) return -1;
    semid = semget(key, 1, 0666);
    if (semid == -1) return -1;
    // Не используем семафор, пока первый процесс его не инициализирует
    for (int attempt = 0; attempt < 300; ++attempt) {
        struct semid_ds ds;
        union { int val; struct semid_ds *buf; unsigned short *array; } arg;
        arg.buf = &ds;
        if (semctl(semid, 0, IPC_STAT, arg) == -1) return -1;
        if (ds.sem_otime != 0) return semid;
        usleep(DELAY_US);
    }
    errno = ETIMEDOUT;
    return -1;
}

// Создаём/подключаем память и проверяем её формат
static struct SharedChat *OpenShared(key_t key, int semid)
{
    int shmid = shmget(key, SHMEM_SIZE, IPC_CREAT | 0666);
    if (shmid == -1) return (void *)-1;
    struct SharedChat *shm = shmat(shmid, NULL, 0);
    if (shm == (void *)-1) return shm;
    if (Lock(semid) == -1) { shmdt(shm); return (void *)-1; }
    // Новая System V память изначально заполнена нулями
    if (shm->magic == 0) {
        shm->bot_enabled = 1;
        shm->magic = CHAT_MAGIC;
    }
    int valid = shm->magic == CHAT_MAGIC;
    Unlock(semid);
    if (!valid) { shmdt(shm); errno = EINVAL; return (void *)-1; }
    return shm;
}

// Регистрируем пользователя, чтобы бот мог проверить его доступность
static int RegisterUser(struct SharedChat *shm, int semid, const char *username)
{
    if (Lock(semid) == -1) return -1;
    int free_index = -1;
    for (int i = 0; i < MAX_USERS; ++i) {
        if (shm->users[i].active &&
            strcmp(shm->users[i].username, username) == 0) {
            Unlock(semid);
            fprintf(stderr, "Username already online\n");
            return -1;
        }
        if (!shm->users[i].active && free_index == -1) free_index = i;
    }
    if (free_index == -1) {
        Unlock(semid);
        fprintf(stderr, "Chat is full\n");
        return -1;
    }
    struct User *user = &shm->users[free_index];
    snprintf(user->username, sizeof(user->username), "%s", username);
    user->pid = getpid();
    user->active = 1;
    Unlock(semid);
    return 0;
}

// Удаляем запись при обычном завершении программы
static void UnregisterUser(struct SharedChat *shm, int semid, const char *username)
{
    if (Lock(semid) == -1) return;
    for (int i = 0; i < MAX_USERS; ++i) {
        struct User *user = &shm->users[i];
        if (user->active && user->pid == getpid() &&
            strcmp(user->username, username) == 0) {
            memset(user, 0, sizeof(*user));
            break;
        }
    }
    Unlock(semid);
}

// Завершение Ctrl+C обрабатывает родитель, чтобы восстановить termios
static volatile sig_atomic_t running = 1;
static void StopChat(int signo) { (void)signo; running = 0; }

// ================= main =================
int main(int argc, char** argv)
{
    if (argc != 2 || !argv[1][0] || strlen(argv[1]) >= USERNAME_MAX ||
        strcmp(argv[1], "Bot") == 0 || strcmp(argv[1], BROADCAST) == 0) {
        fprintf(stderr, "How to use: %s <username> (except Bot or *)\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char* username = argv[1];
    HelloMessage(username);
    int room_id = GetNum("Enter room number: ");

    key_t key = (key_t)((uint32_t)room_id + SHM_KEY_BASE);
    printf("--------------------\n\n");

    int semid = OpenSemaphore(room_id);
    if (semid == -1) { perror("semget"); return EXIT_FAILURE; }

    // грубая проверка заполненности: по users[], а не по shm_nattch
    struct SharedChat *shm = OpenShared(key, semid);
    if (shm == (void *)-1) { perror("OpenShared"); return EXIT_FAILURE; }

    if (RegisterUser(shm, semid, username) == -1) {
        shmdt(shm);
        return EXIT_FAILURE;
    }

    int fd[2];
    if (pipe(fd) == -1) {
        perror("pipe");
        UnregisterUser(shm, semid, username);
        shmdt(shm);
        return EXIT_FAILURE;
    }

    // Создаём процесс чтения сообщений
    pid_t reader_pid = fork();
    if (reader_pid == -1) {
        perror("fork reader");
        close(fd[0]); close(fd[1]);
        UnregisterUser(shm, semid, username);
        shmdt(shm);
        return EXIT_FAILURE;
    }

    if (reader_pid == 0) {
        // Дочерний процесс не должен менять состояние терминала по Ctrl+C
        signal(SIGINT, SIG_IGN);
        close(fd[0]);
        PrintIncome(shm, semid, username, fd[1]);
        close(fd[1]);
        shmdt(shm);
        _exit(EXIT_SUCCESS);
    }

    // Родительский процесс
    close(fd[1]);
    struct sigaction sa = {0};
    sa.sa_handler = StopChat;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    WriteToMem(shm, semid, username, fd[0]);
    close(fd[0]);

    // Завершаем дочерние процессы
    kill(reader_pid, SIGTERM);
    waitpid(reader_pid, NULL, 0);
    UnregisterUser(shm, semid, username);
    shmdt(shm);
    return EXIT_SUCCESS;
}

// ====================funcs========================

void HelloMessage (const char* username)
{
    printf(
        "--------------------\n"
        "SHM-Chat 1.0\n"
        "--------------------\n"
        "User: %s\n"
        "Format: <recipient> <message>\n"
        "  use '%s' as recipient for everyone\n"
        "--------------------\n"
        "stdin:  %s\n"
        "stdout: %s\n"
        "--------------------\n",
    username, BROADCAST, ttyname(fileno(stdin)), ttyname(fileno(stdout)));

    fflush(stdout);
}

  int GetNum(const char* prompt)
{
    char line[64];

    for (;;) {
        printf("%s", prompt);
        fflush(stdout);

        if (fgets(line, sizeof(line), stdin) == NULL) {
            fprintf(stderr, "\nEOF — exiting\n");
            exit(EXIT_FAILURE);
        }

        if (strchr(line, '\n') == NULL) {
            int c; 

			while ((c = getchar()) != '\n' && c != EOF) { }

            fprintf(stderr, "Line too long\n");
            continue;
        }

        char* end; errno = 0;
        long value = strtol(line, &end, 10);

        if (end == line) { 
			fprintf(stderr, "Not a number\n"); continue; 
		}

        while (*end == ' ' || *end == '\t') 
			end++;

        if (*end != '\n' && *end != '\0') { 
			fprintf(stderr, "Garbage after number\n"); 
			continue; 
		}

        if (errno == ERANGE || value < INT_MIN || value > INT_MAX) {
            fprintf(stderr, "Out of int range\n"); 
			continue;
        }

        return (int)value;
    }
}

// "rec text..." → recipient="rec", text="text..." 
int ParseLine(char* line, char* recipient, char* text)
{

    while (*line == ' ' || *line == '\t') {
        line++;
    }

    if (*line == '\0' || *line == '\n') {
        return PARCER_MISTAKE;
    }

    size_t i = 0;

    while (*line != '\0' && *line != ' ' && *line != '\t' && *line != '\n' && i < RECIPIENT_MAX - 1) {
        recipient[i] = *line;
        i++;
        line++;
    }
	
    recipient[i] = '\0';

    if (i == RECIPIENT_MAX - 1
        && *line != ' ' && *line != '\t' && *line != '\n') {
        return PARCER_MISTAKE;
    }

    while (*line == ' ' || *line == '\t') {
        line++;
    }

    size_t j = 0;

    while (*line != '\0' && *line != '\n' && j < TEXT_MAX - 1) {
        text[j] = *line;
        j++;
        line++;
    }

    text[j] = '\0';

    if (i > 0 && j > 0) {
        return PARCER_SUCCESS;
    }

    return PARCER_MISTAKE;
}

// ================= reader ================= 
void PrintIncome(struct SharedChat *shm, int semid, const char* username, int pipe_fd)
{
    usleep(DELAY_US);

    for (;;) {
        for (int i = 0; i < MAX_SLOTS; i++) {
            struct IncomingMessage msg = {0};
            int found = 0;

            if (Lock(semid) == -1) return;
            struct Message *slot = &shm->slots[i];

            // Сообщение можно прочитать, только когда бот его проверил
            if (slot->in_use && slot->checked_by_bot &&
                (strcmp(slot->recipient, username) == 0 ||
                 strcmp(slot->recipient, BROADCAST) == 0)) {

                if (strcmp(slot->sender, username) != 0) {
                    memcpy(msg.sender, slot->sender, USERNAME_MAX);
                    memcpy(msg.recipient, slot->recipient, RECIPIENT_MAX);
                    memcpy(msg.text, slot->message_text, TEXT_MAX);
                    msg.sender[USERNAME_MAX - 1] = '\0';
                    msg.recipient[RECIPIENT_MAX - 1] = '\0';
                    msg.text[TEXT_MAX - 1] = '\0';
                    found = 1;
                }

                // Рассылка * с одним in_use не гарантирует доставку всем
                if (strcmp(slot->sender, username) != 0 ||
                    strcmp(slot->recipient, BROADCAST) != 0)
                    slot->in_use = 0;
            }
            Unlock(semid);

            // Передаём сообщение родителю БЕЗ удержания семафора
            if (found) {
                const char *p = (const char *)&msg;
                size_t remaining = sizeof(msg);
                while (remaining > 0) {
                    ssize_t n = write(pipe_fd, p, remaining);
                    if (n < 0 && errno == EINTR) continue;
                    if (n <= 0) return;
                    p += n;
                    remaining -= (size_t)n;
                }
            }
        }
        usleep(DELAY_US);
    }
}

// ================= writer ================= 
void WriteToMem (struct SharedChat *shm, int semid, const char* username, int pipe_fd)
{
    struct termios old_term;
    if (EnableCharInput(&old_term) == -1) return;

    struct InputBuffer input = {0};
    RedrawInput(&input, username);

    while (running) {
        fd_set readfds;
        if (WaitForInput(pipe_fd, &readfds) < 0) break;
        if (!running) break;
        if (FD_ISSET(pipe_fd, &readfds) &&
            ShowIncoming(pipe_fd, &input, username) == -1) break;
        if (FD_ISSET(STDIN_FILENO, &readfds) &&
            !HandleKey(&input, shm, semid, username)) break;
    }

    tcsetattr(STDIN_FILENO, TCSANOW, &old_term);
    printf("\n");
    fflush(stdout);
}

// Обрабатываем одну нажатую клавишу; возвращаем 0 для выхода из чата
int HandleKey (struct InputBuffer *input, struct SharedChat *shm, int semid, const char *username)
{
    unsigned char c = '\0';
    ssize_t n = 0;
    do {
        n = read(STDIN_FILENO, &c, 1);
    } while (n < 0 && errno == EINTR);

    if (n <= 0) {
        if (n < 0)
            perror("read");
        return 0;
    }

    if (c == '\n' || c == '\r') {
        SubmitInput(input, shm, semid, username);
    } 
    
    else if (c == 127 || c == '\b') {
        EraseLastChar(input);
        RedrawInput(input, username);
    } 
    
    else if (c == 4 && input->len == 0) {
        // CTRL+D на пустой строке
        return 0;
    } 
    
    else if (c >= 32 && c != 127) {
        // Добавляем байт (включая байты UTF-8) в буфер
        if (input->len < sizeof(input->text) - 1) {
            input->text[input->len++] = (char)c;
            input->text[input->len] = '\0';
        }
        RedrawInput(input, username);
    }

    return 1;
}

// Удаляем предыдущий символ (учитываем многобайтовый UTF-8)
void EraseLastChar (struct InputBuffer *input)
{
    if (input->len == 0)
        return;
    input->len--;
    while (input->len > 0 &&
           ((unsigned char)input->text[input->len] & 0xC0) == 0x80)
        input->len--;
    input->text[input->len] = '\0';
}

// Разбираем строку и отправляем сообщение при нажатии Enter
void SubmitInput (struct InputBuffer *input, struct SharedChat *shm, int semid, const char *username)
{
    printf("\r\033[2K\n");
    if (input->len == 0) {
        RedrawInput(input, username);
        return;
    }

    char recipient[RECIPIENT_MAX];
    char text[TEXT_MAX];
    if (ParseLine(input->text, recipient, text) != PARCER_SUCCESS) {
        fprintf(stderr, "Usage: <recipient> <message>\n");
        RedrawInput(input, username);
        return;
    }

    if (SendMessage(shm, semid, username, recipient, text) == 0) {
        // После успешной отправки обнуляем буфер
        input->len = 0;
        input->text[0] = '\0';
    }
    RedrawInput(input, username);
}

// Находим свободный слот и записываем туда сообщение
// ВАЖНО: поиск и запись защищены семафором как одна критическая секция!
int SendMessage (struct SharedChat *shm, int semid, const char *username,
                 const char *recipient, const char *text)
{
    if (Lock(semid) == -1) { perror("Lock"); return -1; }

    // найти свободный слот
    int slot = -1;
    for (int i = 0; i < MAX_SLOTS; i++) {
        if (!shm->slots[i].in_use) { slot = i; break; }
    }

    if (slot == -1) {
        Unlock(semid);
        fprintf(stderr, "Mailbox full, try later\n");
        return -1;
    }

    strncpy(shm->slots[slot].sender, username, USERNAME_MAX - 1);
    strncpy(shm->slots[slot].recipient, recipient, RECIPIENT_MAX - 1);
    strncpy(shm->slots[slot].message_text, text, TEXT_MAX - 1);
    shm->slots[slot].sender[USERNAME_MAX - 1] = '\0';
    shm->slots[slot].recipient[RECIPIENT_MAX - 1] = '\0';
    shm->slots[slot].message_text[TEXT_MAX - 1] = '\0';

    // Пока бот не запущен или поставлен на паузу, чат работает самостоятельно
    shm->slots[slot].checked_by_bot =
        !(shm->bot_active && shm->bot_enabled);
    shm->slots[slot].in_use = 1;

    if (Unlock(semid) == -1) { perror("Unlock"); return -1; }
#ifdef DEBUG
    fprintf(stderr, "[DEBUG Writer] Sent '%s' to '%s' in slot %d\n",
            text, recipient, slot);
#endif
    return 0;
}

// Считываем из pipe одно входящее сообщение и восстанавливаем строку ввода
int ShowIncoming(int pipe_fd, const struct InputBuffer *input, const char *username)
{
    struct IncomingMessage msg = {0};
    size_t received = 0;

    while (received < sizeof(msg)) {
        ssize_t n = read(pipe_fd, (char *)&msg + received,
                         sizeof(msg) - received);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        received += (size_t)n;
    }

    msg.sender[USERNAME_MAX - 1] = '\0';
    msg.recipient[RECIPIENT_MAX - 1] = '\0';
    msg.text[TEXT_MAX - 1] = '\0';

    printf("\r\033[2K[%s -> %s]: %s\n",
           msg.sender, msg.recipient, msg.text);
    RedrawInput(input, username);
    return 0;
}

// Ожидаем событие с клавиатуры или из канала дочернего процесса
int WaitForInput (int pipe_fd, fd_set *readfds)
{
    for (;;) {
        FD_ZERO(readfds);
        FD_SET(STDIN_FILENO, readfds);
        FD_SET(pipe_fd, readfds);

        int maxfd = pipe_fd > STDIN_FILENO ? pipe_fd : STDIN_FILENO;
        int result = select(maxfd + 1, readfds, NULL, NULL, NULL);
        if (result >= 0)
            return result;
        if (errno != EINTR) {
            perror("select");
            return -1;
        }
    }
}

// Настраиваем посимвольный ввод, сохраняя старые настройки терминала
int EnableCharInput (struct termios *old_term)
{
    if (tcgetattr(STDIN_FILENO, old_term) == -1) {
        perror("tcgetattr");
        return -1;
    }

    struct termios new_term = *old_term;
    new_term.c_lflag &= ~(ICANON | ECHO);
    new_term.c_cc[VMIN] = 1;
    new_term.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSANOW, &new_term) == -1) {
        perror("tcsetattr");
        return -1;
    }
    return 0;
}

// ============== reprint input after ===============
void RedrawInput (const struct InputBuffer *input, const char* username)
{
    printf("\r\033[2K[%s] -> %s", username, input->text);
    fflush(stdout);
}