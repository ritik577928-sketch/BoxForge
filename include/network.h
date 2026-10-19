/* network.h - STRETCH: veth + bridge + NAT networking.
 * CORE network isolation (CLONE_NEWNET + loopback) lives in namespaces.c. */
#ifndef BF_NETWORK_H
#define BF_NETWORK_H

#include "container.h"

#define BF_BRIDGE "bfbr0"
#define BF_SUBNET_PREFIX "10.88.0."
#define BF_BRIDGE_IP "10.88.0.1"

/* Parent, before clone: ensure bridge/NAT exist, pick IP and veth names. */
int bf_net_host_prepare(struct container *c);
/* Parent, after clone: create veth pair, move peer into the child's netns,
 * attach the host end to the bridge. */
int bf_net_host_attach(struct container *c);
/* Child, inside the new netns: rename peer to eth0, assign IP, route. */
int bf_net_child_configure(const struct container *c);
/* Remove veth; tear down bridge + NAT when the last container is gone. */
void bf_net_teardown(struct container *c);

#endif
