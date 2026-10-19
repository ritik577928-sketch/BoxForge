/* cpuhog - burn CPU for N wall seconds and report the share of one CPU obtained.
 * usage: cpuhog [seconds]   prints "cpuhog: cpu_share=NN%" */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double now(clockid_t id)
{
    struct timespec ts;
    clock_gettime(id, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    double secs = argc > 1 ? atof(argv[1]) : 4.0;
    double w0 = now(CLOCK_MONOTONIC), c0 = now(CLOCK_PROCESS_CPUTIME_ID);
    volatile unsigned long x = 0;

    while (now(CLOCK_MONOTONIC) - w0 < secs)
        for (int i = 0; i < 100000; i++)
            x += (unsigned long)i;
    printf("cpuhog: cpu_share=%.0f%%\n",
           100.0 * (now(CLOCK_PROCESS_CPUTIME_ID) - c0) / (now(CLOCK_MONOTONIC) - w0));
    return 0;
}
