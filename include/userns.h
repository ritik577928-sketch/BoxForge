/* userns.h - optional user namespace support (CLONE_NEWUSER + uid/gid maps) */
#ifndef BF_USERNS_H
#define BF_USERNS_H

#include "container.h"

bool bf_userns_supported(void);
/* Pick the host uid/gid that container root maps to (flag, SUDO_UID, or 65534). */
void bf_userns_choose_ids(struct container *c);
/* Parent: write setgroups=deny, gid_map and uid_map of the cloned child. */
int bf_userns_write_maps(const struct container *c, pid_t child);
/* Child: switch to the mapped uid/gid 0 once the maps exist. */
int bf_userns_become_root(void);

#endif
