/* fork_tree - build child/grandchild processes and (optionally) leave an
 * orphan to show who reaps it.
 * usage: fork_tree            print the tree, exit 0
 *        fork_tree orphan     leave a dead orphan's zombie check: prints zombies=N */
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int count_zombies(void)
{
    DIR *d = opendir("/proc");
    struct dirent *e;
    int n = 0;

    while (d && (e = readdir(d)) != NULL) {
        char path[300], buf[512];
        FILE *f;
        char *rp;

        if (!isdigit((unsigned char)e->d_name[0]))
            continue;
        snprintf(path, sizeof path, "/proc/%s/stat", e->d_name);
        f = fopen(path, "r");
        if (!f)
            continue;
        if (fgets(buf, sizeof buf, f) && (rp = strrchr(buf, ')')) && rp[2] == 'Z')
            n++;
        fclose(f);
    }
    if (d)
        closedir(d);
    return n;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("fork_tree: root pid=%d ppid=%d\n", getpid(), getppid());
    if (argc > 1 && strcmp(argv[1], "orphan") == 0) {
        pid_t child = fork();
        if (child == 0) {
            if (fork() == 0) { /* grandchild: exits quickly after its parent died */
                usleep(300000);
                _exit(0);
            }
            _exit(0); /* child exits first -> grandchild becomes an orphan */
        }
        waitpid(child, NULL, 0);
        usleep(1200000); /* grandchild exits; who reaps it? */
        printf("fork_tree: zombies=%d (my pid=%d)\n", count_zombies(), getpid());
        return 0;
    }
    pid_t c = fork();
    if (c == 0) {
        pid_t g = fork();
        if (g == 0) {
            printf("fork_tree: grandchild pid=%d ppid=%d\n", getpid(), getppid());
            _exit(0);
        }
        waitpid(g, NULL, 0);
        printf("fork_tree: child pid=%d ppid=%d\n", getpid(), getppid());
        _exit(0);
    }
    waitpid(c, NULL, 0);
    return 0;
}
