/* lifecycle.h - create -> setup -> exec -> monitor -> exit -> cleanup,
 * plus the stop / exec / list / info commands */
#ifndef BF_LIFECYCLE_H
#define BF_LIFECYCLE_H

#include "container.h"

/* Run a fully described container; returns the container's exit status
 * (128+N when killed by signal N, 125 on setup failure). */
int bf_lifecycle_run(struct container *c);
int bf_lifecycle_stop(const char *key);
int bf_lifecycle_exec(const char *key, int argc, char *const argv[]);
int bf_lifecycle_list(void);
int bf_lifecycle_info(const char *key);

#endif
