/* memhog - allocate and touch memory until the cgroup OOM killer stops us.
 * usage: memhog [chunk_mb] [max_mb]  (prints progress; exits 0 if max reached) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    size_t chunk = (argc > 1 ? (size_t)atol(argv[1]) : 8) << 20;
    size_t max = (argc > 2 ? (size_t)atol(argv[2]) : 4096) << 20;
    size_t total = 0;

    setvbuf(stdout, NULL, _IOLBF, 0);
    while (total < max) {
        char *p = malloc(chunk);
        if (p == NULL) {
            puts("memhog: malloc failed");
            return 2;
        }
        memset(p, 0xA5, chunk); /* touch every page so it is really charged */
        total += chunk;
        printf("memhog: allocated %zu MiB\n", total >> 20);
    }
    printf("memhog: reached %zu MiB without being killed\n", total >> 20);
    return 0;
}
