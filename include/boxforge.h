/* boxforge.h - common definitions, logging and small utilities */
#ifndef BOXFORGE_H
#define BOXFORGE_H

#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#define BF_VERSION "1.0.0"
#define BF_DEFAULT_RUNTIME_DIR "/var/run/boxforge"
#define BF_ID_LEN 12
#define BF_NAME_MAX 64
#define BF_HOST_MAX 64
#define BF_MAX_ARGS 128
#define BF_STACK_SIZE (1024 * 1024)
#define BF_DEFAULT_STOP_TIMEOUT 10

/* Exit codes reserved by BoxForge itself (the container's own status is
 * passed through unchanged otherwise). */
#define BF_EXIT_SETUP 125 /* container setup failed                */
#define BF_EXIT_NOEXEC 126 /* command found but could not be run    */
#define BF_EXIT_NOTFOUND 127 /* command not found                     */

enum bf_log_level { BF_LOG_ERROR = 0, BF_LOG_WARN, BF_LOG_INFO, BF_LOG_DEBUG };

void bf_log_set_level(int level);
int bf_log_get_level(void);
const char *bf_last_error(void); /* most recent error message logged */
void bf_log(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#define bf_err(...) bf_log(BF_LOG_ERROR, __VA_ARGS__)
#define bf_warn(...) bf_log(BF_LOG_WARN, __VA_ARGS__)
#define bf_info(...) bf_log(BF_LOG_INFO, __VA_ARGS__)
#define bf_debug(...) bf_log(BF_LOG_DEBUG, __VA_ARGS__)

/* Bounds-checked snprintf: returns 0 on success, -1 (errno=ENAMETOOLONG)
 * if the output would have been truncated. */
int bf_snprintf(char *dst, size_t n, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

ssize_t bf_write_all(int fd, const void *buf, size_t n);
int bf_write_file(const char *path, const char *content);
int bf_read_file(const char *path, char *buf, size_t n);
int bf_mkdir_p(const char *path, mode_t mode);
int bf_rm_rf(const char *path);
bool bf_is_dir(const char *path);
bool bf_path_exists(const char *path);

/* fork/exec a helper without a shell; returns exit status, 128+sig, or -1 */
int bf_run_cmd(char *const argv[], bool quiet);
bool bf_cmd_available(const char *name);

int bf_parse_size(const char *s, unsigned long long *out);
void bf_random_hex(char *out, size_t ndigits);
bool bf_pid_alive(pid_t pid); /* false for dead *and* zombie processes */
void bf_msleep(unsigned ms);

#endif
