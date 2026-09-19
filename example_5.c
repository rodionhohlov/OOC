/* Программа, иллюстрирующая использование системных вызовов open(), read() и close() для чтения информации из файла */

#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char* argv[]) {

    int fd = 0, fd_new = 0;
    size_t size = 0;
    char string[60];

    /* Попытаемся открыть файл с именем в первом параметре выззова только
    для операций чтения */

    if ((fd = open(argv[1], O_RDONLY)) < 0) {
        /* Если файл открыть не удалось, печатаем об этом сообщение и прекращаем работу */
        printf("Can\'t open file\n");
        exit(-1);
    }

    if ((fd_new = open("copy_file.txt", O_WRONLY | O_CREAT | O_TRUNC, 0666)) < 0) {
        printf("Can't open file for writing\n");
        close(fd);
        exit(-1);
    }

    /* Читаем фаил пока не кончится и печатаем */
    while ((size = read(fd, string, 60)) > 0) {
        string[size] = '\0';
        printf("%s\n", string); /* Печатаем прочитанное*/

        write(fd_new, string, size);
    }

    /*  Записываем файл под новым именем */
    if (close(fd) < 0)
        printf("Can\'t close file\n");

    /* Закрываем файл */
    if (close(fd) < 0)
        printf("Can\'t close file\n");

    /*  Открываем файл в редакторе */
    execlp("nano", "nano", "copy_file.txt", NULL);

    return 0;
} 