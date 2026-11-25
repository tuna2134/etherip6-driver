/* SPDX-License-Identifier: GPL-2.0 */
/*
 * EtherIP over IPv6 (RFC 3378) - internal definitions.
 */
#ifndef _ETHERIP6_H
#define _ETHERIP6_H

#include <linux/if_ether.h>
#include <linux/in6.h>
#include <linux/netdevice.h>
#include <linux/rcupdate.h>
#include <linux/types.h>
#include <net/dst_cache.h>
#include <net/gro_cells.h>

#include "etherip6_uapi.h"

/* Per-netns hash table used to find the tunnel for an incoming packet. */
#define ETHERIP6_HASH_BITS	8
#define ETHERIP6_HASH_SIZE	(1U << ETHERIP6_HASH_BITS)
#define ETHERIP6_HASH_SEED	0x9e3779b1U

/* Guards against routing loops through stacked virtual devices. */
#define ETHERIP6_RECURSION_LIMIT	4

/*
 * RFC 3378 EtherIP header:
 *
 *  0                   1
 *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |  Ver  |       Reserved        |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *
 * Ver is 3.  Reserved is transmitted as zero and ignored on receive.
 */
struct etherip_hdr {
	__u8	version;	/* (ETHERIP6_VERSION << 4) | reserved high nibble */
	__u8	reserved;
} __packed;

/* User visible configuration.  Writers must hold RTNL. */
struct etherip6_cfg {
	struct in6_addr	local;
	struct in6_addr	remote;
	__be32		flowlabel;	/* 20 bit label, network byte order */
	u32		link_ifindex;	/* 0: let the routing table decide */
	u32		outer_mtu;	/* 0: use the route MTU */
	u8		hop_limit;
	u8		tclass;
	bool		configured;
};

/*
 * Immutable snapshot of the configuration.  It is installed in the
 * per-netns hash table and published through priv->binding with RCU, so
 * the TX and RX paths never observe a half-updated address.
 */
struct etherip6_binding {
	struct hlist_node	hash;
	struct rcu_head		rcu;
	struct net_device	*dev;
	struct in6_addr		local;
	struct in6_addr		remote;
	__be32			flowlabel;
	u32			link_ifindex;
	u32			outer_mtu;
	u8			hop_limit;
	u8			tclass;
};

struct etherip6_net {
	struct hlist_head	hash[ETHERIP6_HASH_SIZE];
};

struct etherip6_priv {
	struct etherip6_cfg		cfg;		/* RTNL protected */
	struct etherip6_binding __rcu	*binding;	/* RCU / RTNL */
	struct dst_cache		dst_cache;
	struct gro_cells		gro_cells;
};

/* etherip6_main.c */
void etherip6_setup(struct net_device *dev);
int etherip6_bind(struct etherip6_priv *priv, struct net_device *dev);
void etherip6_unbind(struct etherip6_priv *priv);
void etherip6_update_carrier(struct net_device *dev);

/* etherip6_xmit.c */
netdev_tx_t etherip6_xmit(struct sk_buff *skb, struct net_device *dev);

/* etherip6_netlink.c */
extern struct rtnl_link_ops etherip6_link_ops;

#endif /* _ETHERIP6_H */
