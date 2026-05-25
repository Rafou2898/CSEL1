#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MB (1024 * 1024)
#define BLOCK_SIZE (1 * MB)
#define MAX_BLOCKS 1000

int main(void)
{
    void* blocks[MAX_BLOCKS];
    int count = 0;


    while (count < MAX_BLOCKS) {
        blocks[count] = malloc(BLOCK_SIZE);

        if (blocks[count] == NULL) {
            perror("malloc failed");
            break;
        }

        memset(blocks[count], 0, BLOCK_SIZE);

        count++;

        printf("Allocated %d MB\n", count);
        sleep(1);
    }

    printf("Program stopped after allocating %d MB\n", count);

    return 0;
}
