//работает с chat.c
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <limits.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/wait.h>

// ---------- Параметры ----------

#define USERNAME_MAX 32
#define RECIPIENT_MAX 32
#define TEXT_MAX 512
#define MAX_SLOTS 1024
#define MAX_USERS 32
#define BROADCAST "*"
#define DELAY_US 10000

#define SHM_KEY_BASE 0xDEADBABEU
#define SEM_KEY_BASE 0x1BADB002U
#define CHAT_MAGIC 0x43544832U//число для проверкиподключенной памяти 

// ---------- Структуры ----------

struct Message {
    int in_use;
    int checked_by_bot;

    char sender[USERNAME_MAX];
    char recipient[RECIPIENT_MAX];
    char message_text[TEXT_MAX];
};

struct User {
    char username[USERNAME_MAX];
    pid_t pid;
    int active;
};

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

// ---------- Управление завершением ----------

static volatile sig_atomic_t running = 1;

static void StopBot(int signal_number)
{
    (void)signal_number;
    running = 0;
}

// ---------- Ключи IPC ----------

static key_t RoomKey(int room)
{
    return (key_t)((uint32_t)room + SHM_KEY_BASE);
}

static key_t SemKey(int room)
{
    return (key_t)((uint32_t)room + SEM_KEY_BASE);
}

// ---------- Семафоры ----------

// Захватываем семафор
static int Lock(int semid)
{
    struct sembuf operation = {0, -1, SEM_UNDO};

    int result;

    do {
        result = semop(semid, &operation, 1);
    } while (result == -1 && errno == EINTR);

    return result;
}

// Освобождаем семафор
static int Unlock(int semid)
{
    struct sembuf operation = {0, 1, SEM_UNDO};

    int result;

    do {
        result = semop(semid, &operation, 1);
    } while (result == -1 && errno == EINTR);

    return result;
}

// ---------- Проверка пользователя ----------

// Вызывается при захваченном семафоре
static int UserOnline(const struct SharedChat *shm, const char *username)
{
    for (int i = 0; i < MAX_USERS; i++) {

        if (!shm->users[i].active)
            continue;

        if (strcmp(shm->users[i].username, username) == 0)
            return 1;
    }

    return 0;
}

// ---------- Заполнение сообщения ----------

static void FillMessage(struct Message *msg, const char *sender, const char *recipient, const char *text)
{
    memset(msg, 0, sizeof(*msg));

    snprintf(msg->sender, sizeof(msg->sender), "%s", sender);

    snprintf(msg->recipient, sizeof(msg->recipient), "%s", recipient);

    snprintf(msg->message_text, sizeof(msg->message_text), "%s", text);

    msg->checked_by_bot = 1;
    msg->in_use = 1;
}

// ---------- Проверка сообщений ----------

static void CheckMessages(struct SharedChat *shm, int semid)
{
    if (Lock(semid) == -1)
        return;

    if (!shm->bot_enabled) {
        Unlock(semid);
        return;
    }

    for (int i = 0; i < MAX_SLOTS; i++) {

        struct Message *msg = &shm->slots[i];

        // Пропускаем свободный слот
        if (!msg->in_use)
            continue;

        // Пропускаем проверенные сообщения
        if (msg->checked_by_bot)
            continue;

        shm->checked_messages++;

        // Рассылку пока пропускаем без проверки
        if (strcmp(msg->recipient, BROADCAST) == 0) {
            msg->checked_by_bot = 1;
            continue;
        }

        // Получатель подключён
        if (UserOnline(shm, msg->recipient)) {
            msg->checked_by_bot = 1;
            continue;
        }

        // Получатель отсутствует
        char original_sender[USERNAME_MAX];
        char original_recipient[RECIPIENT_MAX];
        char reply[TEXT_MAX];

        snprintf(original_sender, sizeof(original_sender), "%s", msg->sender);

        snprintf(original_recipient, sizeof(original_recipient), "%s", msg->recipient);

        snprintf(reply, sizeof(reply), "User %s is offline", original_recipient);

        // Заменяем исходное сообщение ответом бота
        FillMessage(msg, "Bot", original_sender, reply);

        shm->unavailable_messages++;
    }

    Unlock(semid);
}

// ---------- Цикл бота ----------

static void BotLoop(struct SharedChat *shm, int semid)
{
    for (;;) {
        CheckMessages(shm, semid);
        usleep(DELAY_US);
    }
}

// ---------- Админка ----------

// Показываем пользователей онлайн
static void ShowUsers(struct SharedChat *shm, int semid)
{
    if (Lock(semid) == -1) {
        perror("Lock");
        return;
    }

    printf("\n--- ONLINE USERS ---\n");

    int count = 0;

    for (int i = 0; i < MAX_USERS; i++) {

        if (shm->users[i].active) {

            printf("%s (pid=%ld)\n",
                   shm->users[i].username,
                   (long)shm->users[i].pid);

            count++;
        }
    }

    printf("Total: %d\n", count);

    Unlock(semid);
}

// Показываем статистику
static void ShowStats(struct SharedChat *shm, int semid)
{
    if (Lock(semid) == -1) {
        perror("Lock");
        return;
    }

    int pending = 0;

    for (int i = 0; i < MAX_SLOTS; i++) {

        if (shm->slots[i].in_use &&
            !shm->slots[i].checked_by_bot) {
            pending++;
        }
    }

    printf("\n--- BOT STATISTICS ---\n");

    printf("Status: %s\n",
           shm->bot_enabled ? "ON" : "PAUSED");

    printf("Checked: %u\n", shm->checked_messages);

    printf("Unavailable: %u\n",
           shm->unavailable_messages);

    printf("Pending: %d\n", pending);

    Unlock(semid);
}

// Приостанавливаем или включаем бота
static void SetBotEnabled(struct SharedChat *shm,
                          int semid,
                          int enabled)
{
    if (Lock(semid) == -1) {
        perror("Lock");
        return;
    }

    shm->bot_enabled = enabled;

    // При паузе сообщения должны доставляться
    // обычным способом, без проверки ботом
    if (!enabled) {
        for (int i = 0; i < MAX_SLOTS; i++) {
            if (shm->slots[i].in_use)
                shm->slots[i].checked_by_bot = 1;
        }
    }

    Unlock(semid);

    printf("Bot: %s\n", enabled ? "ON" : "PAUSED");
}

// Меню администратора
static void AdminMenu(struct SharedChat *shm, int semid)
{
    char line[64];

    while (running) {

        printf("\n====== BOT ADMIN ======\n");
        printf("1. Show online users\n");
        printf("2. Show statistics\n");
        printf("3. Pause bot\n");
        printf("4. Resume bot\n");
        printf("5. Exit\n");
        printf("Choose: ");

        fflush(stdout);

        if (fgets(line, sizeof(line), stdin) == NULL)
            break;

        switch (line[0]) {

            case '1':
                ShowUsers(shm, semid);
                break;

            case '2':
                ShowStats(shm, semid);
                break;

            case '3':
                SetBotEnabled(shm, semid, 0);
                break;

            case '4':
                SetBotEnabled(shm, semid, 1);
                break;

            case '5':
                return;

            default:
                printf("Unknown command\n");
        }
    }
}

// ---------- main ----------

int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <room>\n", argv[0]);
        return EXIT_FAILURE;
    }

    char *end;
    errno = 0;

    long room = strtol(argv[1], &end, 10);

    if (errno != 0 || end == argv[1] ||
        *end != '\0' ||
        room < INT_MIN || room > INT_MAX) {

        fprintf(stderr, "Invalid room number\n");
        return EXIT_FAILURE;
    }

    // Подключаемся к существующему семафору
    int semid = semget(SemKey((int)room), 1, 0666);

    if (semid == -1) {
        perror("semget");
        return EXIT_FAILURE;
    }

    // Подключаемся к существующей памяти
    // Без IPC_CREAT!
    int shmid = shmget(
        RoomKey((int)room),
        sizeof(struct SharedChat),
        0666
    );

    if (shmid == -1) {
        perror("shmget");
        return EXIT_FAILURE;
    }

    struct SharedChat *shm = shmat(shmid, NULL, 0);

    if (shm == (void *)-1) {
        perror("shmat");
        return EXIT_FAILURE;
    }

    if (Lock(semid) == -1) {
        perror("Lock");
        shmdt(shm);
        return EXIT_FAILURE;
    }

    // Проверяем версию памяти
    if (shm->magic != CHAT_MAGIC) {
        fprintf(stderr, "Wrong shared memory version\n");
        Unlock(semid);
        shmdt(shm);
        return EXIT_FAILURE;
    }

    // Проверяем наличие пользователей и другого бота
    int users = 0;

    for (int i = 0; i < MAX_USERS; i++) {
        if (shm->users[i].active)
            users++;
    }

    if (users == 0 || shm->bot_active) {
        fprintf(stderr,
                "Chat inactive or bot already running\n");

        Unlock(semid);
        shmdt(shm);
        return EXIT_FAILURE;
    }

    shm->bot_active = 1;
    shm->bot_enabled = 1;
    shm->bot_pid = getpid();

    Unlock(semid);

    // Обработка завершения админки
    struct sigaction sa = {0};
    sa.sa_handler = StopBot;
    sigemptyset(&sa.sa_mask);

    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    // Запускаем процесс бота
    pid_t pid = fork();

    if (pid == -1) {
        perror("fork");
        goto cleanup;
    }

    if (pid == 0) {

        // Бот не читает ввод администратора
        close(STDIN_FILENO);

        // Скрываем вывод дочернего процесса
        FILE *devnull = fopen("/dev/null", "w");

        if (devnull != NULL) {
            dup2(fileno(devnull), STDOUT_FILENO);
            dup2(fileno(devnull), STDERR_FILENO);
            fclose(devnull);
        }

        signal(SIGINT, SIG_IGN);
        signal(SIGTERM, SIG_DFL);

        BotLoop(shm, semid);

        _exit(EXIT_SUCCESS);
    }

    printf("Bot connected to room %ld\n", room);

    // Родительский процесс — админка
    AdminMenu(shm, semid);

    // Завершаем дочерний процесс
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);

cleanup:

    if (Lock(semid) == 0) {

        shm->bot_active = 0;
        shm->bot_pid = 0;

        // После отключения бота чат работает как раньше
        for (int i = 0; i < MAX_SLOTS; i++) {
            if (shm->slots[i].in_use)
                shm->slots[i].checked_by_bot = 1;
        }

        Unlock(semid);
    }

    shmdt(shm);

    return EXIT_SUCCESS;
}
