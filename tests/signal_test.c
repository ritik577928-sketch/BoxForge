/* signal_test - installs SIGTERM/SIGINT handlers, prints READY, waits.
 * Exits 0 after SIGTERM, 3 after SIGINT.  Used to test signal propagation. */
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t got;

static void handler(int sig) { got = sig; }

int main(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = handler;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    printf("signal_test: READY pid=%d\n", getpid());
    fflush(stdout);
    while (!got)
        pause();
    printf("signal_test: got %s\n", got == SIGTERM ? "SIGTERM" : "SIGINT");
    fflush(stdout);
    return got == SIGTERM ? 0 : 3;
}
