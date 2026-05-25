#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sched.h>

void signal_handler(int signum) {
    const char* msg = NULL;
   switch (signum) {
    case SIGHUP:
        msg = "Received SIGHUP signal, ignoring... \n";
        break;
    case SIGINT:
        msg = "Received SIGINT signal, ignoring...\n";
        break;
    case SIGQUIT:
        msg = "Received SIGQUIT signal, ignoring...\n";
        break;
    case SIGABRT:
        msg = "Received SIGABRT signal, ignoring...\n";
        break;
    case SIGTERM:
        msg = "Received SIGTERM signal, ignoring...\n";
        break;
    default:
        msg = "Received unknown signal, ignoring...\n";
        break;
    }
     write(STDOUT_FILENO, msg, strlen(msg));
}

static void pin_to_core(int core)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        perror("sched_setaffinity");
    }
}



void child_process(int fd)
{
    pin_to_core(1);  

    const char* messages[] = {
        "Hello from child!",
        "Second message from child.",
        "Third message from child.",
        "Almost done...",
        "exit"       
    };
    int count = (int)(sizeof(messages) / sizeof(messages[0]));

    for (int i = 0; i < count; i++) {
        printf("Child sends: %s\n", messages[i]);
        fflush(stdout);

        write(fd, messages[i], strlen(messages[i]) + 1);

        if (i < count - 1) {
            sleep(rand() % 2 + 1);
        }
    }
}

void parent_process(int fd)
{
    pin_to_core(0);  

    char buf[256];

    while (1) {
        memset(buf, 0, sizeof(buf));    

        int n = read(fd, buf, sizeof(buf) - 1);
        if (n <= 0) {
            printf("Parent: connection closed, terminating.\n");
            break;
        }

        buf[n] = '\0';                
        printf("Parent received: %s\n", buf);
        fflush(stdout);

        if (strcmp(buf, "exit") == 0) { 
            printf("Parent received exit message, terminating...\n");
            break;
        }
    }
}

int main(void) {

    static struct sigaction sa;
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);


    int sockets[2];

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        perror("cannot create socket pair");
        return -1;
    }

    pid_t pid = fork();

    if (pid == 0) {
        // child code
        close(sockets[0]);
        printf("I am child \n");
        child_process(sockets[1]);
        close(sockets[1]);

    } else if (pid > 0) {
        close(sockets[1]);
        // parent code
        printf("I am parent \n");
        parent_process(sockets[0]);
        close(sockets[0]);
        waitpid(pid, NULL, 0);
    } else {
        perror("fork failed");
        return -1;
    }
    return 0;
}