// SPDX-License-Identifier: GPL-2.0
/*
 * etherip6 - transmit path.
 *
 * A frame is encapsulated as:
 *
 *   +----------------+----------------+---------------------+
 *   | IPv6 header    | EtherIP header | Ethernet frame      |
 *   | 40 bytes       | 2 bytes (0x30  | (including the L2   |
 *   | next header 97 |  0x00)         |  header)            |
 *   +----------------+----------------+---------------------+
 *
 * The IPv6 stack takes care of path MTU discovery and, because
 * skb->ignore_df is set, of IPv6 fragmentation.  Fragmentation is
 * performed by ip6_fragment() (via ip6_local_out()), which is the same
 * well tested code path used by the in-tree IPv6 tunnels.  This makes a
 * 9000 byte inner MTU work over links with a much smaller MTU.
 */
#include <linux/etherdevice.h>
#include <linux/if_ether.h>
#include <linux/if_vlan.h>
#include <linux/ipv6.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/rcupdate.h>
#include <linux/skbuff.h>
#include <linux/types.h>
#include <net/dst.h>
#include <net/ipv6.h>
#include <net/net_namespace.h>

#include "etherip6.h"

netdev_tx_t etherip6_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct etherip6_priv *priv = netdev_priv(dev);
	struct etherip6_binding *binding;
	struct net *net = dev_net(dev);
	struct ipv6hdr *ip6h;
	struct etherip_hdr *eh;
	struct dst_entry *dst;
	struct flowi6 fl6;
	struct in6_addr saddr, daddr;
	u32 link_ifindex;
	u32 outer_mtu;
	unsigned int frame_len = skb->len;
	unsigned int need;
	__be32 flowlabel;
	u8 hop_limit;
	u8 tclass;
	int err;

	/*
	 * The network stack is supposed to honour the device MTU already,
	 * but bridge/raw packet paths can hand us oversized frames.  Allow
	 * one VLAN tag of slack, and always stay below the 16 bit IPv6
	 * payload length limit (no jumbograms are generated).
	 */
	if (unlikely(frame_len < ETH_HLEN ||
		     frame_len > dev->mtu + VLAN_ETH_HLEN ||
		     frame_len > ETHERIP6_MAX_FRAME))
		goto drop;

	if (unlikely(dev_recursion_level() > ETHERIP6_RECURSION_LIMIT)) {
		net_warn_ratelimited("%s: xmit recursion limit reached\n",
				     dev->name);
		goto drop;
	}

	/* Snapshot the RCU protected configuration. */
	rcu_read_lock();
	binding = rcu_dereference(priv->binding);
	if (unlikely(!binding)) {
		rcu_read_unlock();
		goto drop;
	}

	daddr = binding->remote;
	saddr = binding->local;
	link_ifindex = binding->link_ifindex;
	outer_mtu = binding->outer_mtu;
	flowlabel = binding->flowlabel;
	hop_limit = binding->hop_limit;
	tclass = binding->tclass;
	rcu_read_unlock();

	memset(&fl6, 0, sizeof(fl6));
	fl6.flowi6_proto = ETHERIP6_IPPROTO;
	fl6.flowi6_oif = link_ifindex;
	fl6.daddr = daddr;
	fl6.saddr = saddr;
	fl6.flowlabel = flowlabel;

	/*
	 * Cache the route per CPU.  dst_cache validates the entry against
	 * the IPv6 route generation, so route changes (including PMTU
	 * exceptions installed by the routing daemon) are picked up.
	 */
	dst = dst_cache_get(&priv->dst_cache);
	if (unlikely(!dst)) {
		dst = ip6_dst_lookup_flow(net, NULL, &fl6, NULL);
		if (IS_ERR(dst)) {
			net_warn_ratelimited("%s: no route to %pI6c (%ld)\n",
					     dev->name, &fl6.daddr,
					     PTR_ERR(dst));
			dst = NULL;
			goto drop;
		}
		dst_cache_set_ip6(&priv->dst_cache, dst, &fl6.saddr);
	}

	if (unlikely(dst->dev == dev)) {
		net_warn_ratelimited("%s: routing loop to %pI6c\n",
				     dev->name, &fl6.daddr);
		dst_release(dst);
		goto drop;
	}

	/*
	 * Make sure the skb has room for the outer headers *and* for the
	 * link layer header of the egress device.  Keeping enough headroom
	 * also lets locally generated frames travel without a reallocation
	 * once needed_headroom has converged to the real value.
	 */
	need = sizeof(struct ipv6hdr) + ETHERIP6_HLEN +
	       LL_RESERVED_SPACE(dst->dev);
	if (need > READ_ONCE(dev->needed_headroom))
		WRITE_ONCE(dev->needed_headroom, need);

	if (unlikely(skb_cow_head(skb, need))) {
		dst_release(dst);
		goto drop;
	}

	/*
	 * Do not skb_orphan() here: the skb may still own MSG_ZEROCOPY
	 * pages and its destructor must only run when the skb is really
	 * freed by the egress path.
	 *
	 * The inner socket's PMTU policy nevertheless cannot leak into the
	 * outer packet: ip6_skb_dst_mtu() and ip6_fragment() ignore
	 * skb->sk while dev_recursion_level() is non-zero, and we run the
	 * whole output path between dev_xmit_recursion_inc()/dec() below.
	 */
	skb_scrub_packet(skb, false);

	eh = skb_push(skb, ETHERIP6_HLEN);
	eh->version = ETHERIP6_VERSION << 4;
	eh->reserved = 0;

	ip6h = skb_push(skb, sizeof(*ip6h));
	skb_reset_network_header(skb);
	ip6_flow_hdr(ip6h, tclass, flowlabel);
	ip6h->payload_len = htons((u16)(ETHERIP6_HLEN + frame_len));
	ip6h->nexthdr = ETHERIP6_IPPROTO;
	ip6h->hop_limit = hop_limit;
	ip6h->saddr = fl6.saddr;
	ip6h->daddr = fl6.daddr;

	skb_dst_set(skb, dst);

	/*
	 * EtherIP carries an opaque L2 frame, so fragmenting the outer IPv6
	 * packet is always safe and transparent.  Tell the IPv6 output path
	 * that it may fragment even though there is no socket / DF flag.
	 */
	skb->ignore_df = 1;

	/* The inner frame's control block must not leak into IPv6 output. */
	memset(skb->cb, 0, sizeof(struct inet6_skb_parm));

	/*
	 * Optional outer MTU cap.  ip6_fragment() honours
	 * IP6CB()->frag_max_size, which lets an operator bound the outer
	 * fragment size even when no ICMPv6 PTB is available for protocol
	 * 97.  Always clamp to the real route MTU first.
	 */
	if (outer_mtu) {
		u32 cap = min(outer_mtu, (u32)dst_mtu(dst));

		IP6CB(skb)->frag_max_size = (u16)cap;
	}

	dev_sw_netstats_tx_add(dev, 1, frame_len);

	dev_xmit_recursion_inc();
	err = ip6_local_out(net, NULL, skb);
	dev_xmit_recursion_dec();

	if (unlikely(err))
		DEV_STATS_INC(dev, tx_errors);

	return NETDEV_TX_OK;

drop:
	DEV_STATS_INC(dev, tx_dropped);
	dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;
}
