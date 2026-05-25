#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>

void burn_cpu(void)
{
    unsigned long long counter = 0;

    while (1) {
        counter++;
    }
}

int main(void)
{
    printf("Parent PID = %d\n", getpid());

    pid_t pid = fork();

    if (pid < 0) {
        perror("fork");
        return 1;
    }

    if (pid == 0) {
        printf("Child PID = %d\n", getpid());
        burn_cpu();
    } else {
        printf("Parrent PID = %d\n", getpid());
        burn_cpu();
    }

    return 0;
}