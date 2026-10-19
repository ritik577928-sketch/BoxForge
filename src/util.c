/* util.c - logging and small helpers shared by all modules */
#include "boxforge.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* The log level is the one piece of process-wide state (set once by the CLI). */
static int g_log_level = BF_LOG_WARN;
static char g_last_error[1024] = "";

const char *bf_last_error(void) { return g_last_error; }

void bf_log_set_level(int level) { g_log_level = level; }
int bf_log_get_level(void) { return g_log_level; }

void bf_log(int level, const char *fmt, ...)
{
    static const char *const tag[] = {"error", "warn", "info", "debug"};
    char msg[1024];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (level == BF_LOG_ERROR)
        snprintf(g_last_error, sizeof g_last_error, "%s", msg);
    if (level > g_log_level)
        return;
    fprintf(stderr, "boxforge: %s: %s\n", tag[level], msg);
}

int bf_snprintf(char *dst, size_t n, const char *fmt, ...)
{
    va_list ap;
    int r;

    va_start(ap, fmt);
    r = vsnprintf(dst, n, fmt, ap);
    va_end(ap);
    if (r < 0 || (size_t)r >= n) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

ssize_t bf_write_all(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    size_t done = 0;

    while (done < n) {
        ssize_t w = write(fd, p + done, n - done);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        done += (size_t)w;
    }
    return (ssize_t)done;
}

int bf_write_file(const char *path, const char *content)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    int saved;

    if (fd < 0)
        return -1;
    if (bf_write_all(fd, content, strlen(content)) < 0) {
        saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return close(fd);
}

int bf_read_file(const char *path, char *buf, size_t n)
{
    int fd;
    ssize_t r;

    if (n == 0) {
        errno = EINVAL;
        return -1;
    }
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    do {
        r = read(fd, buf, n - 1);
    } while (r < 0 && errno == EINTR);
    if (r < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    close(fd);
    buf[r] = '\0';
    return (int)r;
}

int bf_mkdir_p(const char *path, mode_t mode)
{
    char buf[PATH_MAX];
    size_t i;

    if (bf_snprintf(buf, sizeof buf, "%s", path) < 0)
        return -1;
    for (i = 1; buf[i] != '\0'; i++) {
        if (buf[i] != '/')
            continue;
        buf[i] = '\0';
        if (mkdir(buf, mode) < 0 && errno != EEXIST)
            return -1;
        buf[i] = '/';
    }
    if (mkdir(buf, mode) < 0 && errno != EEXIST)
        return -1;
    return bf_is_dir(buf) ? 0 : (errno = ENOTDIR, -1);
}

static int rm_cb(const char *p, const struct stat *sb, int type, struct FTW *f)
{
    (void)sb;
    (void)type;
    (void)f;
    if (remove(p) < 0 && errno != ENOENT)
        bf_debug("remove(%s): %s", p, strerror(errno));
    return 0;
}

int bf_rm_rf(const char *path)
{
    struct stat st;

    if (lstat(path, &st) < 0)
        return errno == ENOENT ? 0 : -1;
    /* FTW_MOUNT: never descend into other filesystems (safety net). */
    if (nftw(path, rm_cb, 32, FTW_DEPTH | FTW_PHYS | FTW_MOUNT) < 0)
        return -1;
    return lstat(path, &st) < 0 && errno == ENOENT ? 0 : -1;
}

bool bf_is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

bool bf_path_exists(const char *path)
{
    struct stat st;
    return lstat(path, &st) == 0;
}

int bf_run_cmd(char *const argv[], bool quiet)
{
    pid_t pid = fork();
    int status;

    if (pid < 0)
        return -1;
    if (pid == 0) {
        if (quiet) {
            int fd = open("/dev/null", O_WRONLY);
            if (fd >= 0) {
                dup2(fd, STDOUT_FILENO);
                dup2(fd, STDERR_FILENO);
            }
        }
        execvp(argv[0], argv);
        _exit(127);
    }
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            return -1;
    }
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1;
}

bool bf_cmd_available(const char *name)
{
    const char *path = getenv("PATH");
    char *copy, *save = NULL, *dir;
    char full[PATH_MAX];
    bool found = false;

    if (path == NULL)
        path = "/usr/sbin:/usr/bin:/sbin:/bin";
    copy = strdup(path);
    if (copy == NULL)
        return false;
    for (dir = strtok_r(copy, ":", &save); dir != NULL; dir = strtok_r(NULL, ":", &save)) {
        if (bf_snprintf(full, sizeof full, "%s/%s", dir, name) == 0 && access(full, X_OK) == 0) {
            found = true;
            break;
        }
    }
    free(copy);
    return found;
}

int bf_parse_size(const char *s, unsigned long long *out)
{
    char *end = NULL;
    unsigned long long v, mult = 1;

    if (s == NULL || *s == '\0' || !isdigit((unsigned char)*s)) {
        errno = EINVAL;
        return -1;
    }
    errno = 0;
    v = strtoull(s, &end, 10);
    if (errno != 0)
        return -1;
    switch (toupper((unsigned char)*end)) {
    case '\0':
        break;
    case 'K':
        mult = 1ULL << 10;
        end++;
        break;
    case 'M':
        mult = 1ULL << 20;
        end++;
        break;
    case 'G':
        mult = 1ULL << 30;
        end++;
        break;
    case 'T':
        mult = 1ULL << 40;
        end++;
        break;
    default:
        errno = EINVAL;
        return -1;
    }
    if (strcasecmp(end, "") != 0 && strcasecmp(end, "b") != 0 && strcasecmp(end, "ib") != 0) {
        errno = EINVAL;
        return -1;
    }
    if (v > ULLONG_MAX / mult) {
        errno = ERANGE;
        return -1;
    }
    *out = v * mult;
    return 0;
}

void bf_random_hex(char *out, size_t ndigits)
{
    static const char hex[] = "0123456789abcdef";
    unsigned char rnd[32];
    size_t i;

    if (ndigits > sizeof rnd * 2)
        ndigits = sizeof rnd * 2;
    if (getrandom(rnd, sizeof rnd, 0) != (ssize_t)sizeof rnd) {
        for (i = 0; i < sizeof rnd; i++)
            rnd[i] = (unsigned char)(rand() & 0xff);
    }
    for (i = 0; i < ndigits; i++)
        out[i] = hex[(rnd[i / 2] >> ((i % 2) ? 0 : 4)) & 0xf];
    out[ndigits] = '\0';
}

bool bf_pid_alive(pid_t pid)
{
    char path[64], buf[512];
    char *rp;

    if (pid <= 0)
        return false;
    if (kill(pid, 0) < 0 && errno != EPERM)
        return false;
    if (bf_snprintf(path, sizeof path, "/proc/%d/stat", (int)pid) < 0)
        return true;
    if (bf_read_file(path, buf, sizeof buf) < 0)
        return false;
    rp = strrchr(buf, ')');
    if (rp == NULL || rp[1] == '\0' || rp[2] == '\0')
        return true;
    return rp[2] != 'Z' && rp[2] != 'X';
}

void bf_msleep(unsigned ms)
{
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
        ;
}
