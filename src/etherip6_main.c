// SPDX-License-Identifier: GPL-2.0
/*
 * etherip6 - EtherIP (RFC 3378) over IPv6 network driver.
 *
 * The driver implements a virtual Ethernet (ARPHRD_ETHER) device.  Every
 * frame written to it is encapsulated into an IPv6 packet with next header
 * 97 (EtherIP) and a two byte RFC 3378 header (version 3), then handed to
 * the IPv6 stack.  Incoming EtherIP packets are matched against the
 * configured (local, remote) endpoint pairs and injected into the matching
 * netdevice.
 *
 * Only IPv6 outer addresses are supported, as requested.  The inner frames
 * can carry anything Ethernet can carry (IPv4, IPv6, ARP, VLAN, ...).
 *
 * The netdevice is configured through rtnetlink (see etherip6_netlink.c),
 * which keeps the driver independent from patched iproute2 versions.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/etherdevice.h>
#include <linux/if_ether.h>
#include <linux/init.h>
#include <linux/jhash.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/rcupdate.h>
#include <linux/skbuff.h>
#include <linux/types.h>
#include <net/gro_cells.h>
#include <net/ipv6.h>
#include <net/net_namespace.h>
#include <net/netns/generic.h>
#include <net/rtnetlink.h>
#include <net/protocol.h>

#include "etherip6.h"

static unsigned int etherip6_net_id;

static inline u32 etherip6_bucket(const struct in6_addr *local,
				  const struct in6_addr *remote)
{
	u32 hash;

	hash = jhash2((const u32 *)local, 4, ETHERIP6_HASH_SEED);
	return jhash2((const u32 *)remote, 4, hash) &
	       (ETHERIP6_HASH_SIZE - 1);
}

/*
 * Install a new immutable binding for @dev.  Must be called with RTNL held.
 *
 * A fresh binding is allocated before the old one is unlinked, so the RCU
 * readers (TX and RX) always see a complete and consistent configuration.
 * The old binding is freed after a grace period.
 */
int etherip6_bind(struct etherip6_priv *priv, struct net_device *dev)
{
	struct etherip6_binding *new = NULL, *old;
	struct etherip6_net *en;

	if (priv->cfg.configured) {
		new = kzalloc(sizeof(*new), GFP_KERNEL);
		if (!new)
			return -ENOMEM;

		new->dev = dev;
		new->local = priv->cfg.local;
		new->remote = priv->cfg.remote;
		new->flowlabel = priv->cfg.flowlabel;
		new->link_ifindex = priv->cfg.link_ifindex;
		new->outer_mtu = priv->cfg.outer_mtu;
		new->hop_limit = priv->cfg.hop_limit;
		new->tclass = priv->cfg.tclass;
	}

	en = net_generic(dev_net(dev), etherip6_net_id);

	/* Publish first, then retire the old binding: RX never sees a hole. */
	old = rtnl_dereference(priv->binding);
	rcu_assign_pointer(priv->binding, new);

	if (new)
		hlist_add_head_rcu(&new->hash,
				   &en->hash[etherip6_bucket(&new->local,
							     &new->remote)]);

	if (old) {
		hlist_del_rcu(&old->hash);
		kfree_rcu(old, rcu);
	}

	return 0;
}

/* Remove the binding.  Must be called with RTNL held. */
void etherip6_unbind(struct etherip6_priv *priv)
{
	struct etherip6_binding *old = rtnl_dereference(priv->binding);

	if (!old)
		return;

	rcu_assign_pointer(priv->binding, NULL);
	hlist_del_rcu(&old->hash);
	kfree_rcu(old, rcu);
}

void etherip6_update_carrier(struct net_device *dev)
{
	struct etherip6_priv *priv = netdev_priv(dev);

	if (priv->cfg.configured && netif_running(dev))
		netif_carrier_on(dev);
	else
		netif_carrier_off(dev);
}

static int etherip6_dev_init(struct net_device *dev)
{
	struct etherip6_priv *priv = netdev_priv(dev);
	int err;

	err = dst_cache_init(&priv->dst_cache, GFP_KERNEL);
	if (err)
		return err;

	err = gro_cells_init(&priv->gro_cells, dev);
	if (err)
		goto err_dst_cache;

	err = etherip6_bind(priv, dev);
	if (err)
		goto err_gro_cells;

	return 0;

err_gro_cells:
	gro_cells_destroy(&priv->gro_cells);
err_dst_cache:
	dst_cache_destroy(&priv->dst_cache);
	return err;
}

static void etherip6_dev_uninit(struct net_device *dev)
{
	struct etherip6_priv *priv = netdev_priv(dev);

	etherip6_unbind(priv);
	gro_cells_destroy(&priv->gro_cells);
	dst_cache_destroy(&priv->dst_cache);
}

static int etherip6_dev_open(struct net_device *dev)
{
	struct etherip6_priv *priv = netdev_priv(dev);

	if (priv->cfg.configured)
		netif_carrier_on(dev);
	else
		netif_carrier_off(dev);

	return 0;
}

static int etherip6_dev_stop(struct net_device *dev)
{
	netif_carrier_off(dev);
	return 0;
}

/*
 * Receive path.  Called from the IPv6 input path with the RCU read lock
 * held and skb->data pointing at the EtherIP header.
 */
static int etherip6_rcv(struct sk_buff *skb)
{
	struct etherip6_binding *binding, *match = NULL, *wildcard = NULL;
	struct etherip6_net *en;
	struct etherip6_priv *priv;
	struct net_device *dev;
	const struct ipv6hdr *ip6h;
	struct etherip_hdr *eh;
	u32 bucket;

	if (unlikely(!pskb_may_pull(skb, ETHERIP6_HLEN)))
		goto drop;

	eh = (struct etherip_hdr *)skb->data;
	if (unlikely((eh->version >> 4) != ETHERIP6_VERSION))
		goto drop;

	ip6h = ipv6_hdr(skb);
	en = net_generic(dev_net(skb->dev), etherip6_net_id);
	bucket = etherip6_bucket(&ip6h->daddr, &ip6h->saddr);

	rcu_read_lock();

	hlist_for_each_entry_rcu(binding, &en->hash[bucket], hash) {
		if (!ipv6_addr_equal(&binding->local, &ip6h->daddr) ||
		    !ipv6_addr_equal(&binding->remote, &ip6h->saddr))
			continue;

		if (!binding->link_ifindex) {
			if (!wildcard)
				wildcard = binding;
		} else if (binding->link_ifindex == skb->dev->ifindex) {
			match = binding;
			break;
		}
	}

	if (!match)
		match = wildcard;

	if (unlikely(!match)) {
		rcu_read_unlock();
		goto drop;
	}

	dev = match->dev;
	if (unlikely(!netif_running(dev))) {
		DEV_STATS_INC(dev, rx_dropped);
		rcu_read_unlock();
		goto drop;
	}

	priv = netdev_priv(dev);

	skb_pull(skb, ETHERIP6_HLEN);
	if (unlikely(!pskb_may_pull(skb, ETH_HLEN))) {
		DEV_STATS_INC(dev, rx_length_errors);
		DEV_STATS_INC(dev, rx_dropped);
		rcu_read_unlock();
		goto drop;
	}

	/*
	 * Scrub before eth_type_trans(): skb_scrub_packet() forces
	 * pkt_type to PACKET_HOST, and we want the L2 classification
	 * (broadcast/multicast/otherhost) computed from the inner header.
	 */
	skb_scrub_packet(skb, false);

	skb->protocol = eth_type_trans(skb, dev);
	skb_postpull_rcsum(skb, eth_hdr(skb), ETH_HLEN);
	skb_reset_network_header(skb);

	/*
	 * Keep CHECKSUM_PARTIAL if it survived the trip.  This happens when
	 * the outer path is virtual (e.g. veth) and claims checksum offload
	 * but has no hardware to complete the inner checksum.  The inner
	 * stack can complete it using csum_start/csum_offset exactly like it
	 * does for veth.  Forcing CHECKSUM_NONE here would make the inner
	 * TCP/UDP stack verify an uncomputed checksum and drop the traffic.
	 *
	 * Frames received from a real wire are CHECKSUM_NONE (or
	 * CHECKSUM_UNNECESSARY for the outer header); for those the inner
	 * checksum is already present and must be verified, so set
	 * CHECKSUM_NONE.
	 */
	if (skb->ip_summed != CHECKSUM_PARTIAL)
		skb->ip_summed = CHECKSUM_NONE;

	/* The outer IPv6 control block is stale from here on. */
	memset(skb->cb, 0, sizeof(struct inet6_skb_parm));

	dev_sw_netstats_rx_add(dev, skb->len);

	gro_cells_receive(&priv->gro_cells, skb);

	rcu_read_unlock();
	return 0;

drop:
	kfree_skb(skb);
	return 0;
}

static const struct net_device_ops etherip6_netdev_ops = {
	.ndo_init		= etherip6_dev_init,
	.ndo_uninit		= etherip6_dev_uninit,
	.ndo_open		= etherip6_dev_open,
	.ndo_stop		= etherip6_dev_stop,
	.ndo_start_xmit		= etherip6_xmit,
	.ndo_get_stats64	= dev_get_tstats64,
	.ndo_set_mac_address	= eth_mac_addr,
	.ndo_validate_addr	= eth_validate_addr,
};

void etherip6_setup(struct net_device *dev)
{
	ether_setup(dev);

	dev->netdev_ops = &etherip6_netdev_ops;

	/*
	 * A pure software L2 tunnel: no qdisc (tx_queue_len 0 + NO_QUEUE)
	 * and no xmit lock (LLTX), the driver is reentrant.
	 */
	dev->priv_flags &= ~IFF_TX_SKB_SHARING;
	dev->priv_flags |= IFF_NO_QUEUE | IFF_LIVE_ADDR_CHANGE;
	dev->tx_queue_len = 0;

	dev->features |= NETIF_F_LLTX | NETIF_F_SG | NETIF_F_HW_CSUM;
	dev->hw_features |= NETIF_F_SG | NETIF_F_HW_CSUM;

	/*
	 * Do not allow the device to be moved between network namespaces:
	 * the RX lookup table is per-netns and the netns is part of the
	 * endpoint identity.
	 */
	dev->features |= NETIF_F_NETNS_LOCAL;

	/*
	 * Locally generated frames get enough headroom to prepend the outer
	 * IPv6 + EtherIP headers plus the worst case link layer header.
	 */
	dev->needed_headroom = sizeof(struct ipv6hdr) + ETHERIP6_HLEN +
			       LL_MAX_HEADER;

	/*
	 * Ask the core to allocate dev->tstats (pcpu_sw_netstats) for us.
	 * Without this, dev_get_tstats64()/dev_sw_netstats_*_add() would
	 * dereference a NULL per-CPU pointer and corrupt unrelated per-CPU
	 * data.
	 */
	dev->pcpu_stat_type = NETDEV_PCPU_STAT_TSTATS;

	dev->mtu = ETHERIP6_DEFAULT_MTU;
	dev->min_mtu = ETHERIP6_MIN_MTU;
	dev->max_mtu = ETHERIP6_MAX_MTU;

	eth_hw_addr_random(dev);
}

static const struct inet6_protocol etherip6_protocol = {
	.handler	= etherip6_rcv,
	.flags		= INET6_PROTO_NOPOLICY | INET6_PROTO_FINAL,
};

static int etherip6_net_init(struct net *net);

static struct pernet_operations etherip6_net_ops = {
	.id	= &etherip6_net_id,
	.size	= sizeof(struct etherip6_net),
	.init	= etherip6_net_init,
};

static int etherip6_net_init(struct net *net)
{
	struct etherip6_net *en = net_generic(net, etherip6_net_id);
	int i;

	for (i = 0; i < ETHERIP6_HASH_SIZE; i++)
		INIT_HLIST_HEAD(&en->hash[i]);

	return 0;
}

static int __init etherip6_init(void)
{
	int err;

	err = inet6_add_protocol(&etherip6_protocol, ETHERIP6_IPPROTO);
	if (err) {
		pr_err("cannot register IPv6 protocol %d: %d\n",
		       ETHERIP6_IPPROTO, err);
		return err;
	}

	err = register_pernet_subsys(&etherip6_net_ops);
	if (err)
		goto err_protocol;

	err = rtnl_link_register(&etherip6_link_ops);
	if (err)
		goto err_pernet;

	pr_info("EtherIP over IPv6 driver loaded (RFC 3378, IP protocol %d)\n",
		ETHERIP6_IPPROTO);
	return 0;

err_pernet:
	unregister_pernet_subsys(&etherip6_net_ops);
err_protocol:
	inet6_del_protocol(&etherip6_protocol, ETHERIP6_IPPROTO);
	return err;
}

static void __exit etherip6_exit(void)
{
	/* Stop the RX path first, then tear the devices down. */
	inet6_del_protocol(&etherip6_protocol, ETHERIP6_IPPROTO);
	rtnl_link_unregister(&etherip6_link_ops);
	unregister_pernet_subsys(&etherip6_net_ops);
	pr_info("EtherIP over IPv6 driver unloaded\n");
}

module_init(etherip6_init);
module_exit(etherip6_exit);

MODULE_DESCRIPTION("EtherIP (RFC 3378) over IPv6 tunnel driver");
MODULE_AUTHOR("etherip6 contributors");
MODULE_LICENSE("GPL");
MODULE_ALIAS_RTNL_LINK(ETHERIP6_KIND);
MODULE_VERSION("1.0.0");
