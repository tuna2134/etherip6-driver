// SPDX-License-Identifier: GPL-2.0
/*
 * etherip6 - rtnetlink configuration.
 *
 * The endpoint configuration is part of IFLA_INFO_DATA for the
 * "etherip6" link kind, i.e. plain rtnetlink:
 *
 *   ip link add eip0 type etherip6 \
 *          remote 2001:db8::2 local 2001:db8::1
 *
 * or, without a patched iproute2, through the bundled etherip6ctl tool.
 * Using rtnetlink (instead of a private genetlink family) means that
 * device creation/removal, namespaces, statistics and "ip link" handling
 * all come for free from the kernel's standard link infrastructure.
 */
#include <linux/if_ether.h>
#include <linux/in6.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/skbuff.h>
#include <linux/types.h>
#include <net/ipv6.h>
#include <net/netlink.h>
#include <net/rtnetlink.h>

#include "etherip6.h"

static int etherip6_validate_cfg(struct etherip6_cfg *cfg,
				 struct netlink_ext_ack *extack)
{
	bool local_any = ipv6_addr_any(&cfg->local);
	bool remote_any = ipv6_addr_any(&cfg->remote);

	if (local_any && remote_any) {
		cfg->configured = false;
		return 0;
	}

	if (local_any || remote_any) {
		NL_SET_ERR_MSG(extack,
			       "local and remote IPv6 addresses must be configured together");
		return -EINVAL;
	}

	if (ipv6_addr_type(&cfg->local) & IPV6_ADDR_MULTICAST) {
		NL_SET_ERR_MSG(extack, "local address must be unicast");
		return -EINVAL;
	}

	if (ipv6_addr_type(&cfg->remote) & IPV6_ADDR_MULTICAST) {
		NL_SET_ERR_MSG(extack, "remote address must be unicast");
		return -EINVAL;
	}

	/*
	 * Link local endpoints have no meaning without an egress device:
	 * the routing table cannot pick a scope zone for them.
	 */
	if (!cfg->link_ifindex &&
	    ((ipv6_addr_type(&cfg->local) & IPV6_ADDR_LINKLOCAL) ||
	     (ipv6_addr_type(&cfg->remote) & IPV6_ADDR_LINKLOCAL))) {
		NL_SET_ERR_MSG(extack,
			       "link local endpoint addresses require a link device");
		return -EINVAL;
	}

	if (!cfg->hop_limit)
		cfg->hop_limit = ETHERIP6_DEFAULT_HOP_LIMIT;

	cfg->configured = true;
	return 0;
}

static int etherip6_parse_attrs(struct net *net, struct net_device *dev,
				struct nlattr **data, struct etherip6_cfg *cfg,
				struct netlink_ext_ack *extack)
{
	if (data) {
		if (data[IFLA_ETHERIP6_REMOTE])
			nla_memcpy(&cfg->remote, data[IFLA_ETHERIP6_REMOTE],
				   sizeof(cfg->remote));

		if (data[IFLA_ETHERIP6_LOCAL])
			nla_memcpy(&cfg->local, data[IFLA_ETHERIP6_LOCAL],
				   sizeof(cfg->local));

		if (data[IFLA_ETHERIP6_LINK]) {
			u32 ifindex = nla_get_u32(data[IFLA_ETHERIP6_LINK]);
			struct net_device *link_dev;

			if (ifindex) {
				link_dev = dev_get_by_index(net, ifindex);
				if (!link_dev) {
					NL_SET_ERR_MSG_FMT(extack,
							   "link device with ifindex %u does not exist",
							   ifindex);
					return -ENODEV;
				}
				if (link_dev == dev) {
					dev_put(link_dev);
					NL_SET_ERR_MSG(extack,
						       "link device must not be the tunnel device itself");
					return -EINVAL;
				}
				dev_put(link_dev);
			}
			cfg->link_ifindex = ifindex;
		}

		if (data[IFLA_ETHERIP6_HOP_LIMIT])
			cfg->hop_limit =
				nla_get_u8(data[IFLA_ETHERIP6_HOP_LIMIT]) ? :
				ETHERIP6_DEFAULT_HOP_LIMIT;

		if (data[IFLA_ETHERIP6_TCLASS])
			cfg->tclass = nla_get_u8(data[IFLA_ETHERIP6_TCLASS]);

		if (data[IFLA_ETHERIP6_OUTER_MTU]) {
			u32 outer_mtu =
				nla_get_u32(data[IFLA_ETHERIP6_OUTER_MTU]);

			if (outer_mtu && (outer_mtu < IPV6_MIN_MTU ||
					  outer_mtu > 65535)) {
				NL_SET_ERR_MSG_FMT(extack,
						   "outer MTU %u is out of range (1280..65535, or 0)",
						   outer_mtu);
				return -EINVAL;
			}
			cfg->outer_mtu = outer_mtu;
		}

		if (data[IFLA_ETHERIP6_FLOWLABEL]) {
			u32 flowlabel =
				nla_get_u32(data[IFLA_ETHERIP6_FLOWLABEL]);

			if (flowlabel & ~ETHERIP6_MAX_FLOWLABEL) {
				NL_SET_ERR_MSG_FMT(extack,
						   "flow label %#x is out of range",
						   flowlabel);
				return -EINVAL;
			}
			cfg->flowlabel = htonl(flowlabel);
		}
	}

	return etherip6_validate_cfg(cfg, extack);
}

static int etherip6_newlink(struct net *src_net, struct net_device *dev,
			    struct nlattr *tb[], struct nlattr *data[],
			    struct netlink_ext_ack *extack)
{
	struct etherip6_priv *priv = netdev_priv(dev);
	struct etherip6_cfg cfg = {
		.hop_limit = ETHERIP6_DEFAULT_HOP_LIMIT,
	};
	int err;

	(void)src_net;
	(void)tb;

	err = etherip6_parse_attrs(dev_net(dev), dev, data, &cfg, extack);
	if (err)
		return err;

	priv->cfg = cfg;

	/*
	 * This is a rtnl_link_ops with a newlink() callback, so we own
	 * the registration.  ndo_init() will publish the binding.
	 */
	err = register_netdevice(dev);
	if (err)
		return err;

	etherip6_update_carrier(dev);
	return 0;
}

static int etherip6_changelink(struct net_device *dev, struct nlattr *tb[],
			       struct nlattr *data[],
			       struct netlink_ext_ack *extack)
{
	struct etherip6_priv *priv = netdev_priv(dev);
	struct etherip6_cfg cfg = priv->cfg;
	struct etherip6_cfg old_cfg;
	int err;

	(void)tb;

	err = etherip6_parse_attrs(dev_net(dev), dev, data, &cfg, extack);
	if (err)
		return err;

	old_cfg = priv->cfg;
	priv->cfg = cfg;

	err = etherip6_bind(priv, dev);
	if (err) {
		priv->cfg = old_cfg;
		return err;
	}

	/* The route (and possibly the source address) may have changed. */
	dst_cache_reset(&priv->dst_cache);
	etherip6_update_carrier(dev);

	return 0;
}

static void etherip6_dellink(struct net_device *dev, struct list_head *head)
{
	unregister_netdevice_queue(dev, head);
}

static size_t etherip6_get_size(const struct net_device *dev)
{
	(void)dev;

	return nla_total_size(sizeof(struct in6_addr)) +	/* REMOTE */
	       nla_total_size(sizeof(struct in6_addr)) +	/* LOCAL */
	       nla_total_size(sizeof(u32)) +			/* LINK */
	       nla_total_size(sizeof(u8)) +			/* HOP_LIMIT */
	       nla_total_size(sizeof(u8)) +			/* TCLASS */
	       nla_total_size(sizeof(u32)) +			/* FLOWLABEL */
	       nla_total_size(sizeof(u32));			/* OUTER_MTU */
}

static int etherip6_fill_info(struct sk_buff *skb, const struct net_device *dev)
{
	const struct etherip6_priv *priv = netdev_priv(dev);

	if (nla_put(skb, IFLA_ETHERIP6_REMOTE, sizeof(priv->cfg.remote),
		    &priv->cfg.remote) ||
	    nla_put(skb, IFLA_ETHERIP6_LOCAL, sizeof(priv->cfg.local),
		    &priv->cfg.local) ||
	    nla_put_u32(skb, IFLA_ETHERIP6_LINK, priv->cfg.link_ifindex) ||
	    nla_put_u8(skb, IFLA_ETHERIP6_HOP_LIMIT, priv->cfg.hop_limit) ||
	    nla_put_u8(skb, IFLA_ETHERIP6_TCLASS, priv->cfg.tclass) ||
	    nla_put_u32(skb, IFLA_ETHERIP6_FLOWLABEL,
			ntohl(priv->cfg.flowlabel)) ||
	    nla_put_u32(skb, IFLA_ETHERIP6_OUTER_MTU, priv->cfg.outer_mtu))
		return -EMSGSIZE;

	return 0;
}

static const struct nla_policy etherip6_policy[IFLA_ETHERIP6_MAX + 1] = {
	[IFLA_ETHERIP6_REMOTE] =
		NLA_POLICY_EXACT_LEN(sizeof(struct in6_addr)),
	[IFLA_ETHERIP6_LOCAL] =
		NLA_POLICY_EXACT_LEN(sizeof(struct in6_addr)),
	[IFLA_ETHERIP6_LINK] = { .type = NLA_U32 },
	[IFLA_ETHERIP6_HOP_LIMIT] = { .type = NLA_U8 },
	[IFLA_ETHERIP6_TCLASS] = { .type = NLA_U8 },
	[IFLA_ETHERIP6_FLOWLABEL] = { .type = NLA_U32 },
	[IFLA_ETHERIP6_OUTER_MTU] = { .type = NLA_U32 },
};

struct rtnl_link_ops etherip6_link_ops __read_mostly = {
	.kind		= ETHERIP6_KIND,
	.priv_size	= sizeof(struct etherip6_priv),
	.setup		= etherip6_setup,
	.maxtype	= IFLA_ETHERIP6_MAX,
	.policy		= etherip6_policy,
	.newlink	= etherip6_newlink,
	.changelink	= etherip6_changelink,
	.dellink	= etherip6_dellink,
	.get_size	= etherip6_get_size,
	.fill_info	= etherip6_fill_info,
};
