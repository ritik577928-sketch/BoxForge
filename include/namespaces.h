/* namespaces.h - namespace flags, UTS/loopback setup, setns() entry,
 * capability dropping and the container environment */
#ifndef BF_NAMESPACES_H
#define BF_NAMESPACES_H

#include "container.h"

/* CLONE_NEWPID|NEWNS|NEWUTS|NEWIPC|NEWNET (+NEWUSER) | SIGCHLD */
int bf_ns_clone_flags(const struct container *c);
int bf_ns_set_hostname(const char *name);
int bf_ns_loopback_up(void);
/* Drop dangerous capabilities from the bounding set (PR_CAPBSET_DROP). */
int bf_ns_drop_capabilities(void);
void bf_ns_setup_env(const struct container *c);

/* Open every namespace of <pid>, then setns() into them (user first, pid
 * last). *joined_user is set when a different user namespace was entered. */
int bf_ns_join(pid_t pid, bool *joined_user);

#endif
