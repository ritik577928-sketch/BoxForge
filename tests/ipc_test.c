/* ipc_test - SysV shared memory visibility, to prove IPC namespace isolation.
 * usage: ipc_test create KEY | check KEY | remove KEY      (KEY is an integer)
 * check: exit 0 and print "found" if the segment is visible, else exit 1. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>

int main(int argc, char **argv)
{
    key_t key;
    int id;

    if (argc != 3) {
        fprintf(stderr, "usage: ipc_test create|check|remove KEY\n");
        return 2;
    }
    key = (key_t)strtol(argv[2], NULL, 0);
    if (!strcmp(argv[1], "create")) {
        id = shmget(key, 4096, IPC_CREAT | 0600);
        if (id < 0) {
            perror("shmget");
            return 1;
        }
        printf("created shmid=%d key=%d\n", id, (int)key);
        return 0;
    }
    id = shmget(key, 0, 0);
    if (!strcmp(argv[1], "check")) {
        if (id < 0) {
            printf("not found (%s)\n", strerror(errno));
            return 1;
        }
        printf("found shmid=%d\n", id);
        return 0;
    }
    if (!strcmp(argv[1], "remove")) {
        if (id >= 0 && shmctl(id, IPC_RMID, NULL) == 0) {
            puts("removed");
            return 0;
        }
        return 1;
    }
    return 2;
}
