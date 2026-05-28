#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define SOCKET_PATH "/tmp/fan.sock"

static void print_usage(const char* progname) {
    fprintf(stderr, "Usage: %s <command> <value>\n", progname);
    fprintf(stderr, "  mode <0|1>     -> 0=AUTO, 1=MANUAL\n");
    fprintf(stderr, "  freq <0|1|2|3> -> 0=2Hz, 1=5Hz, 2=10Hz, 3=20Hz\n");
}

static int validate_args(int argc, char* argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Error: missing arguments\n");
        return -1;
    }

    if (strcmp(argv[1], "mode") != 0 && strcmp(argv[1], "freq") != 0) {
        fprintf(stderr, "Error: unknown command '%s'\n", argv[1]);
        return -1;
    }

    int val = atoi(argv[2]);
    if (strcmp(argv[1], "mode") == 0 && (val < 0 || val > 1)) {
        fprintf(stderr, "Error: mode value must be 0 or 1\n");
        return -1;
    }
    if (strcmp(argv[1], "freq") == 0 && (val < 0 || val > 3)) {
        fprintf(stderr, "Error: freq value must be 0, 1, 2 or 3\n");
        return -1;
    }

    return 0;
}

int main(int argc, char* argv[]) {
    if (validate_args(argc, argv) != 0) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd == -1) {
        perror("socket");
        return EXIT_FAILURE;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        perror("connect");
        close(fd);
        return EXIT_FAILURE;
    }

    char msg[64];
    snprintf(msg, sizeof(msg), "%s %s", argv[1], argv[2]);
    if (write(fd, msg, strlen(msg)) == -1) {
        perror("write");
        close(fd);
        return EXIT_FAILURE;
    }

    char response[256];
    ssize_t n = read(fd, response, sizeof(response) - 1);
    if (n > 0) {
        response[n] = '\0';
        printf("%s\n", response);
    } else if (n == -1) {
        perror("read");
    }

    close(fd);
    return 0;
}