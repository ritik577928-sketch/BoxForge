/* cgroup.h - cgroup v2 resource control (memory.max, cpu.max only; no v1) */
#ifndef BF_CGROUP_H
#define BF_CGROUP_H

#include "container.h"

int bf_cgroup_detect_root(char *out, size_t n);
int bf_parse_cpu_percent(const char *s, unsigned *pct);
int bf_parse_memory(const char *s, unsigned long long *bytes);

/* Create /<cgroup2>/boxforge/<id> and write memory.max / cpu.max. */
int bf_cgroup_create(struct container *c);
int bf_cgroup_add_pid(const struct container *c, pid_t pid);
unsigned long bf_cgroup_oom_kills(const struct container *c);
void bf_cgroup_remove(struct container *c);
void bf_cgroup_cleanup_parent(void);

#endif
