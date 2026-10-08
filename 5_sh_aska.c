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

#define DEBUG

// ---------- параметры ----------
#define USERNAME_MAX 32
#define RECIPIENT_MAX 32
#define TEXT_MAX 512
#define MAX_SLOTS 1024 //лимит участников
#define BROADCAST "*" //спецсимвол трансляции для всех

enum { 
	DELAY_US = 10 * 1000, //  10 мс 
	PARCER_MISTAKE = -1,
	PARCER_SUCCESS = 0
};  

static const unsigned int SHM_KEY_BASE = 0xDEADBABEU;

// ---------- структуры ---------- 
struct Message {
    volatile int in_use; // свобоный слот, читаемый только из памяти
    char sender[USERNAME_MAX];
    char recipient[RECIPIENT_MAX];
    char message_text[TEXT_MAX];
};

struct SharedChat {
    struct Message slots[MAX_SLOTS];
};

static const size_t SHMEM_SIZE = sizeof(struct SharedChat);

static void HelloMessage (const char* username);

static int  GetNum       (const char* prompt);

static int  ParseLine    (char* line, char* recipient, char* text);

static void PrintIncome  (key_t key, const char* username);

static void WriteToMem   (key_t key, const char* username);

// ================= main =================
int main(int argc, char** argv)
{
    if (argc != 2) {
        fprintf(stderr, "How to use: %s <username>\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char* username = argv[1];

    HelloMessage(username);

    int room_id = GetNum("Enter room number: ");

    key_t key = (key_t)((uint32_t)room_id + SHM_KEY_BASE);
    printf("--------------------\n\n");

    // грубая проверка заполненности 
    int test = shmget(key, SHMEM_SIZE, IPC_CREAT | 0666);

    if (test == -1) {
        perror("shmget failed");
        return EXIT_FAILURE;
    }

    struct shmid_ds buf;

    if (shmctl(test, IPC_STAT, &buf) == -1) {
		perror("shmctl");
	}
		
	if (buf.shm_nattch >= MAX_SLOTS) {
		fprintf(stderr, "Chat is full (%lu participants). Exit process...\n", (unsigned long)buf.shm_nattch);
		return EXIT_FAILURE;
	}

	// пногопроцессорное общение
    pid_t pid = fork();

    if (pid == -1) { 
		perror("fork"); 
		return EXIT_FAILURE; 
	}

    if (pid == 0) {
        PrintIncome(key, username);
    } 
	
	else {
        WriteToMem(key, username);
    }

    return EXIT_SUCCESS;
}

// ====================funcs========================

static void HelloMessage (const char* username)
{
    char tty_in[64] = "unknown", tty_out[64] = "unknown";

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

static int GetNum(const char* prompt)
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
static void PrintIncome(const key_t key, const char* username)
{
    usleep(DELAY_US);

    int shmid = shmget(key, SHMEM_SIZE, IPC_CREAT | 0666);

    if (shmid == -1) { 
		perror("shmget"); 
		exit(EXIT_FAILURE); 
	}

    struct SharedChat* shm = shmat(shmid, NULL, 0);

    if (shm == (void*)-1) { 
		perror("shmat"); 
		exit(EXIT_FAILURE); 
	}

    for (;;) {
        for (int i = 0; i < MAX_SLOTS; i++) {
            if (shm->slots[i].in_use == 0) 
				continue;

            if (strcmp(shm->slots[i].recipient, username) != 0 && strcmp(shm->slots[i].recipient, BROADCAST) != 0)
                continue;

            if (strcmp(shm->slots[i].sender, username) == 0) {
                shm->slots[i].in_use = 0;
                continue;
            }

            printf("[%s -> %s]: %s\n", shm->slots[i].sender, shm->slots[i].recipient, shm->slots[i].message_text);

            fflush(stdout);

            shm->slots[i].in_use = 0;
        }
        usleep(DELAY_US);
    }
}

// ================= writer ================= 
void WriteToMem(const key_t key, const char* username)
{
    int shmid = shmget(key, SHMEM_SIZE, IPC_CREAT | 0666);

    if (shmid == -1) { 
		perror("shmget"); 
		exit(EXIT_FAILURE); 
	}

    struct SharedChat* shm = shmat(shmid, NULL, 0);

    if (shm == (void*)-1) { 
		perror("shmat"); 
		exit(EXIT_FAILURE); 
	}

    char line[TEXT_MAX + RECIPIENT_MAX + 8];

    for (;;) {

        fflush(stdout);

        if (fgets(line, sizeof(line), stdin) == NULL) {
			break;
		}

        char recipient[RECIPIENT_MAX];
        char text[TEXT_MAX];

        if (ParseLine(line, recipient, text) != PARCER_SUCCESS) {
            fprintf(stderr, "Usage: <recipient> <message>\n");
            continue;
        }

        // найти свободный слот 
        int slot = -1;

        for (int i = 0; i < MAX_SLOTS; i++) {
            if (!shm->slots[i].in_use) { 
				slot = i; break; 
			}
        }

        if (slot == -1) {
            fprintf(stderr, "Mailbox full, try later\n");
            continue;
        }

        strncpy(shm->slots[slot].sender, username, USERNAME_MAX - 1);
        strncpy(shm->slots[slot].recipient, recipient, RECIPIENT_MAX - 1);
        strncpy(shm->slots[slot].message_text, text, TEXT_MAX - 1);

        shm->slots[slot].sender[USERNAME_MAX - 1] = '\0';
        shm->slots[slot].recipient[RECIPIENT_MAX - 1] = '\0';
        shm->slots[slot].message_text[TEXT_MAX - 1] = '\0';
        shm->slots[slot].in_use = 1;

    #ifdef DEBUG
        fprintf(stderr, "[DEBUG Writer] Sent '%s' to '%s' in slot %d\n", text, recipient, slot);
    #endif
    }
}