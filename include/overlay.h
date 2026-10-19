/* overlay.h - OverlayFS root: lower (read-only base) + upper + work = merged */
#ifndef BF_OVERLAY_H
#define BF_OVERLAY_H

#include "container.h"

/* Parent: create upper/work/merged directories (rootfs becomes the lower). */
int bf_overlay_prepare(struct container *c);
/* Child (inside its own mount namespace): mount the overlay on merged. */
int bf_overlay_mount(const struct container *c);
void bf_overlay_cleanup(struct container *c);

#endif
