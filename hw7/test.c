#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <errno.h>

#define DEVICE "/dev/pipebuf0"
#define MSG_SIZE 64
#define MSG_COUNT 10000

void writer(char fill) {
    int fd = open(DEVICE, O_WRONLY);
    if (fd < 0) {
        perror("open writer error");
        exit(1);
    }

    char buf[MSG_SIZE];
    memset(buf, fill, sizeof(buf));

    for (int i = 0; i < MSG_COUNT; i++) {
        ssize_t ret = write(fd, buf, sizeof(buf));
        if (ret < 0) {
            perror("write error");
            exit(1);
        }
    }

    close(fd);
    exit(0);
}

void reader() {
    int fd = open(DEVICE, O_RDONLY);
    if (fd < 0) {
        perror("open reader error");
        exit(1);
    }

    char buf[MSG_SIZE];

    while (1) {
        ssize_t ret = read(fd, buf, sizeof(buf));
        if (ret == 0) {
            break;
        }
        if (ret < 0) {
            perror("read error");
            exit(1);
        }

        char c = buf[0];
        for (int i = 1; i < ret; i++) {
            if (buf[i] != c) {
                printf("Unexpected behaviour!\n");
                exit(1);
            }
        }
    }

    close(fd);
}

int main() {
    if (fork() == 0) writer('A');
    if (fork() == 0) writer('B');

    reader();

    wait(NULL);
    wait(NULL);

    printf("Test completed\n");
}