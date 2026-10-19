/* rootfs.h - root filesystem preparation: pivot_root, /proc, /dev */
#ifndef BF_ROOTFS_H
#define BF_ROOTFS_H

#include "container.h"

/* Resolve and sanity-check the rootfs and (absolute) command. */
int bf_rootfs_resolve(struct container *c, const char *path);
/* Child, first mount operation: make every mount private (no host leaks). */
int bf_rootfs_make_private(void);
/* Child: bind root onto itself, populate /dev, mount /proc. */
int bf_rootfs_prepare(const char *root);
/* Child: pivot_root into root and detach the old root. */
int bf_rootfs_pivot(const char *root);
/* Debug fallback only - NOT a full isolation mechanism. */
int bf_rootfs_chroot(const char *root);

#endif
