/* clone_runtime.h - clone() the container process and run its child side */
#ifndef BF_CLONE_RUNTIME_H
#define BF_CLONE_RUNTIME_H

#include "container.h"

/* Allocate the child stack, clone() with the namespace flags and release the
 * stack again.  On success c->host_pid is the container init's host PID. */
int bf_clone_container(struct container *c);

#endif
