// SPDX-License-Identifier: GPL-2.0
/*
 * etherip6ctl - configure etherip6 (EtherIP over IPv6) tunnel devices.
 *
 * The kernel driver exposes its configuration through standard rtnetlink
 * (IFLA_LINKINFO/IFLA_INFO_DATA with the "etherip6" kind).  This tool
 * speaks rtnetlink directly, so it does not depend on a patched iproute2.
 */
#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <linux/if_ether.h>
#include <linux/if_link.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "etherip6_uapi.h"

#define NL_BUFSIZE	32768
#define REQ_BUFSIZE	2048

static unsigned int nl_seq = 1;
static int nl_fd = -1;

static void usage(FILE *out)
{
	fprintf(out,
"usage: etherip6ctl create <name> [remote <ipv6>] [local <ipv6>]\n"
"                            [link <dev|ifindex>] [hoplimit <n>]\n"
"                            [tclass <n>] [flowlabel <n>] [outer-mtu <n>]\n"
"                            [mtu <n>] [up]\n"
"       etherip6ctl set <name> [remote <ipv6>] [local <ipv6>]\n"
"                            [link <dev|ifindex>] [hoplimit <n>]\n"
"                            [tclass <n>] [flowlabel <n>] [outer-mtu <n>]\n"
"                            [mtu <n>] [up|down]\n"
"       etherip6ctl show [<name>]\n"
"       etherip6ctl del <name>\n"
"\n"
"RFC 3378 EtherIP: Ethernet frames are carried in IPv6 packets with\n"
"next header 97.  Both endpoint addresses are mandatory before the\n"
"device can pass traffic.\n");
}

struct opts {
	const char	*name;
	bool		has_remote;
	bool		has_local;
	bool		has_link;
	bool		has_hop;
	bool		has_tclass;
	bool		has_flow;
	bool		has_outer_mtu;
	bool		has_mtu;
	int		updown;		/* 0: unchanged, 1: up, -1: down */
	struct in6_addr	remote;
	struct in6_addr	local;
	unsigned int	link;
	unsigned int	hop;
	unsigned int	tclass;
	unsigned int	flow;
	unsigned int	outer_mtu;
	unsigned int	mtu;
};

static int parse_u32(const char *s, unsigned int max, unsigned int *out)
{
	char *end;
	unsigned long v;

	errno = 0;
	v = strtoul(s, &end, 0);
	if (errno || *end || end == s || v > max)
		return -1;

	*out = (unsigned int)v;
	return 0;
}

static int parse_link(const char *s, unsigned int *out)
{
	unsigned int idx;

	if (parse_u32(s, UINT32_MAX, &idx) == 0) {
		*out = idx;
		return 0;
	}

	idx = if_nametoindex(s);
	if (!idx)
		return -1;

	*out = idx;
	return 0;
}

static int parse_opts(int argc, char **argv, struct opts *o)
{
	int i;

	memset(o, 0, sizeof(*o));

	if (argc < 2)
		return -1;

	o->name = argv[1];

	for (i = 2; i < argc; i++) {
		const char *key = argv[i];
		const char *val;

		if (!strcmp(key, "up")) {
			o->updown = 1;
			continue;
		}
		if (!strcmp(key, "down")) {
			o->updown = -1;
			continue;
		}

		if (i + 1 >= argc)
			return -1;
		val = argv[++i];

		if (!strcmp(key, "remote")) {
			if (inet_pton(AF_INET6, val, &o->remote) != 1)
				return -1;
			o->has_remote = true;
		} else if (!strcmp(key, "local")) {
			if (inet_pton(AF_INET6, val, &o->local) != 1)
				return -1;
			o->has_local = true;
		} else if (!strcmp(key, "link")) {
			if (parse_link(val, &o->link) < 0)
				return -1;
			o->has_link = true;
		} else if (!strcmp(key, "hoplimit")) {
			if (parse_u32(val, 255, &o->hop) < 0)
				return -1;
			o->has_hop = true;
		} else if (!strcmp(key, "tclass")) {
			if (parse_u32(val, 255, &o->tclass) < 0)
				return -1;
			o->has_tclass = true;
		} else if (!strcmp(key, "flowlabel")) {
			if (parse_u32(val, ETHERIP6_MAX_FLOWLABEL,
				      &o->flow) < 0)
				return -1;
			o->has_flow = true;
		} else if (!strcmp(key, "outer-mtu")) {
			if (parse_u32(val, 65535, &o->outer_mtu) < 0)
				return -1;
			o->has_outer_mtu = true;
		} else if (!strcmp(key, "mtu")) {
			if (parse_u32(val, ETHERIP6_MAX_MTU, &o->mtu) < 0)
				return -1;
			o->has_mtu = true;
		} else {
			return -1;
		}
	}

	return 0;
}

static void nl_open(void)
{
	struct sockaddr_nl sa = {
		.nl_family = AF_NETLINK,
	};

	nl_fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
	if (nl_fd < 0) {
		perror("socket(NETLINK_ROUTE)");
		exit(1);
	}

	if (bind(nl_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		perror("bind(NETLINK_ROUTE)");
		exit(1);
	}
}

static void addattr_l(struct nlmsghdr *n, int maxlen, int type,
		      const void *data, int alen)
{
	int len = RTA_LENGTH(alen);
	struct rtattr *rta;

	if ((int)NLMSG_ALIGN(n->nlmsg_len) + (int)RTA_ALIGN(len) > maxlen) {
		fprintf(stderr, "etherip6ctl: netlink request too large\n");
		exit(1);
	}

	rta = (struct rtattr *)((char *)n + NLMSG_ALIGN(n->nlmsg_len));
	rta->rta_type = type;
	rta->rta_len = len;
	if (alen)
		memcpy(RTA_DATA(rta), data, alen);
	n->nlmsg_len = NLMSG_ALIGN(n->nlmsg_len) + RTA_ALIGN(len);
}

static struct rtattr *addattr_nest(struct nlmsghdr *n, int maxlen, int type)
{
	struct rtattr *nest;

	nest = (struct rtattr *)((char *)n + NLMSG_ALIGN(n->nlmsg_len));
	addattr_l(n, maxlen, type, NULL, 0);
	return nest;
}

static void addattr_nest_end(struct nlmsghdr *n, struct rtattr *nest)
{
	nest->rta_len = (char *)n + NLMSG_ALIGN(n->nlmsg_len) - (char *)nest;
}

static void add_uint8(struct nlmsghdr *n, int maxlen, int type, unsigned int v)
{
	uint8_t val = (uint8_t)v;

	addattr_l(n, maxlen, type, &val, sizeof(val));
}

static void add_uint32(struct nlmsghdr *n, int maxlen, int type, unsigned int v)
{
	uint32_t val = (uint32_t)v;

	addattr_l(n, maxlen, type, &val, sizeof(val));
}

static void print_extack(const struct nlmsgerr *err, int payload_len)
{
	const struct nlattr *attr;
	int len;

	if (payload_len <= (int)sizeof(*err))
		return;

	len = payload_len - (int)sizeof(*err);
	attr = (const struct nlattr *)((const char *)err + sizeof(*err));

	while (len >= (int)sizeof(*attr) && attr->nla_len >= sizeof(*attr) &&
	       attr->nla_len <= len) {
		/* NLMSGERR_ATTR_MSG */
		if ((attr->nla_type & NLA_TYPE_MASK) == 1)
			fprintf(stderr, "etherip6ctl: %s\n",
				(const char *)attr + NLA_HDRLEN);
		len -= NLA_ALIGN(attr->nla_len);
		attr = (const struct nlattr *)((const char *)attr +
					       NLA_ALIGN(attr->nla_len));
	}
}

static int nl_wait_ack(void)
{
	char buf[NL_BUFSIZE];

	for (;;) {
		ssize_t len = recv(nl_fd, buf, sizeof(buf), 0);
		struct nlmsghdr *nlh;

		if (len < 0) {
			if (errno == EINTR)
				continue;
			perror("recv(netlink)");
			return -errno;
		}

		for (nlh = (struct nlmsghdr *)buf; NLMSG_OK(nlh, len);
		     nlh = NLMSG_NEXT(nlh, len)) {
			struct nlmsgerr *err;

			if (nlh->nlmsg_type != NLMSG_ERROR)
				continue;

			err = (struct nlmsgerr *)NLMSG_DATA(nlh);
			if (err->error == 0)
				return 0;

			print_extack(err, nlh->nlmsg_len -
					  NLMSG_LENGTH(sizeof(*err)));
			fprintf(stderr, "etherip6ctl: %s\n",
				strerror(-err->error));
			return err->error;
		}
	}
}

static int nl_talk(struct nlmsghdr *nlh)
{
	struct sockaddr_nl sa = {
		.nl_family = AF_NETLINK,
	};
	struct iovec iov = {
		.iov_base = nlh,
		.iov_len = nlh->nlmsg_len,
	};
	struct msghdr msg = {
		.msg_name = &sa,
		.msg_namelen = sizeof(sa),
		.msg_iov = &iov,
		.msg_iovlen = 1,
	};

	nlh->nlmsg_seq = nl_seq++;

	if (sendmsg(nl_fd, &msg, 0) < 0) {
		perror("sendmsg(netlink)");
		return -errno;
	}

	return nl_wait_ack();
}

struct link_state {
	char			ifname[IFNAMSIZ];
	int			ifindex;
	unsigned int		mtu;
	unsigned int		operstate;
	bool			has_addr;
	unsigned char		addr[ETH_ALEN];
	bool			is_etherip6;
	bool			configured;
	struct in6_addr		local;
	struct in6_addr		remote;
	unsigned int		link;
	unsigned int		hop;
	unsigned int		tclass;
	unsigned int		flow;
	unsigned int		outer_mtu;
	bool			has_stats;
	struct rtnl_link_stats64 stats;
};

static int parse_attrs(struct rtattr **tb, int max, struct rtattr *rta,
		       int len)
{
	memset(tb, 0, sizeof(*tb) * (max + 1));

	while (RTA_OK(rta, len)) {
		if (rta->rta_type <= max)
			tb[rta->rta_type] = rta;
		rta = RTA_NEXT(rta, len);
	}

	return 0;
}

static void parse_link_msg(struct nlmsghdr *nlh, struct link_state *st)
{
	struct ifinfomsg *ifm = (struct ifinfomsg *)NLMSG_DATA(nlh);
	int len = IFLA_PAYLOAD(nlh);
	struct rtattr *tb[IFLA_MAX + 1];

	memset(st, 0, sizeof(*st));
	parse_attrs(tb, IFLA_MAX, IFLA_RTA(ifm), len);

	st->ifindex = ifm->ifi_index;

	if (tb[IFLA_IFNAME])
		snprintf(st->ifname, sizeof(st->ifname), "%s",
			 (char *)RTA_DATA(tb[IFLA_IFNAME]));

	if (tb[IFLA_MTU] && RTA_PAYLOAD(tb[IFLA_MTU]) >= sizeof(uint32_t))
		st->mtu = *(uint32_t *)RTA_DATA(tb[IFLA_MTU]);

	if (tb[IFLA_OPERSTATE] &&
	    RTA_PAYLOAD(tb[IFLA_OPERSTATE]) >= sizeof(uint8_t))
		st->operstate = *(uint8_t *)RTA_DATA(tb[IFLA_OPERSTATE]);

	if (tb[IFLA_ADDRESS] &&
	    RTA_PAYLOAD(tb[IFLA_ADDRESS]) >= ETH_ALEN) {
		memcpy(st->addr, RTA_DATA(tb[IFLA_ADDRESS]), ETH_ALEN);
		st->has_addr = true;
	}

	if (tb[IFLA_STATS64] &&
	    RTA_PAYLOAD(tb[IFLA_STATS64]) >= sizeof(st->stats)) {
		memcpy(&st->stats, RTA_DATA(tb[IFLA_STATS64]),
		       sizeof(st->stats));
		st->has_stats = true;
	}

	if (tb[IFLA_LINKINFO]) {
		struct rtattr *li[IFLA_INFO_MAX + 1];

		parse_attrs(li, IFLA_INFO_MAX, RTA_DATA(tb[IFLA_LINKINFO]),
			    RTA_PAYLOAD(tb[IFLA_LINKINFO]));

		if (li[IFLA_INFO_KIND]) {
			const char *kind = RTA_DATA(li[IFLA_INFO_KIND]);

			if (!strcmp(kind, ETHERIP6_KIND))
				st->is_etherip6 = true;
		}

		if (st->is_etherip6 && li[IFLA_INFO_DATA]) {
			struct rtattr *d[IFLA_ETHERIP6_MAX + 1];

			parse_attrs(d, IFLA_ETHERIP6_MAX,
				    RTA_DATA(li[IFLA_INFO_DATA]),
				    RTA_PAYLOAD(li[IFLA_INFO_DATA]));

			if (d[IFLA_ETHERIP6_REMOTE] &&
			    RTA_PAYLOAD(d[IFLA_ETHERIP6_REMOTE]) >=
				    sizeof(struct in6_addr))
				memcpy(&st->remote,
				       RTA_DATA(d[IFLA_ETHERIP6_REMOTE]),
				       sizeof(st->remote));

			if (d[IFLA_ETHERIP6_LOCAL] &&
			    RTA_PAYLOAD(d[IFLA_ETHERIP6_LOCAL]) >=
				    sizeof(struct in6_addr))
				memcpy(&st->local,
				       RTA_DATA(d[IFLA_ETHERIP6_LOCAL]),
				       sizeof(st->local));

			if (d[IFLA_ETHERIP6_LINK])
				st->link = *(uint32_t *)
					RTA_DATA(d[IFLA_ETHERIP6_LINK]);

			if (d[IFLA_ETHERIP6_HOP_LIMIT])
				st->hop = *(uint8_t *)
					RTA_DATA(d[IFLA_ETHERIP6_HOP_LIMIT]);

			if (d[IFLA_ETHERIP6_TCLASS])
				st->tclass = *(uint8_t *)
					RTA_DATA(d[IFLA_ETHERIP6_TCLASS]);

			if (d[IFLA_ETHERIP6_FLOWLABEL])
				st->flow = *(uint32_t *)
					RTA_DATA(d[IFLA_ETHERIP6_FLOWLABEL]);

			if (d[IFLA_ETHERIP6_OUTER_MTU])
				st->outer_mtu = *(uint32_t *)
					RTA_DATA(d[IFLA_ETHERIP6_OUTER_MTU]);

			st->configured =
				!(IN6_IS_ADDR_UNSPECIFIED(&st->local) &&
				  IN6_IS_ADDR_UNSPECIFIED(&st->remote));
		}
	}
}

static const char *operstate_str(unsigned int state)
{
	switch (state) {
	case 0: return "UNKNOWN";
	case 1: return "NOTPRESENT";
	case 2: return "DOWN";
	case 3: return "LOWERLAYERDOWN";
	case 4: return "TESTING";
	case 5: return "DORMANT";
	case 6: return "UP";
	default: return "?";
	}
}

static void print_addr(const char *label, const struct in6_addr *addr)
{
	char buf[INET6_ADDRSTRLEN];

	if (!inet_ntop(AF_INET6, addr, buf, sizeof(buf)))
		snprintf(buf, sizeof(buf), "<invalid>");
	printf("  %-8s %s\n", label, buf);
}

static void print_link(const struct link_state *st)
{
	printf("%s (ifindex %d): mtu %u, state %s, mac ",
	       st->ifname, st->ifindex, st->mtu,
	       operstate_str(st->operstate));

	if (st->has_addr) {
		printf("%02x:%02x:%02x:%02x:%02x:%02x\n",
		       st->addr[0], st->addr[1], st->addr[2],
		       st->addr[3], st->addr[4], st->addr[5]);
	} else {
		printf("none\n");
	}

	if (!st->configured) {
		printf("  (unconfigured)\n");
	} else {
		print_addr("local", &st->local);
		print_addr("remote", &st->remote);
		printf("  link     %u\n", st->link);
		printf("  outer    hoplimit %u, tclass %u, flowlabel 0x%05x, mtu %u\n",
		       st->hop, st->tclass, st->flow, st->outer_mtu);
	}

	if (st->has_stats) {
		printf("  rx       %llu packets, %llu bytes, %llu errors, %llu dropped\n",
		       (unsigned long long)st->stats.rx_packets,
		       (unsigned long long)st->stats.rx_bytes,
		       (unsigned long long)st->stats.rx_errors,
		       (unsigned long long)st->stats.rx_dropped);
		printf("  tx       %llu packets, %llu bytes, %llu errors, %llu dropped\n",
		       (unsigned long long)st->stats.tx_packets,
		       (unsigned long long)st->stats.tx_bytes,
		       (unsigned long long)st->stats.tx_errors,
		       (unsigned long long)st->stats.tx_dropped);
	}
}

static int get_link_by_index(int ifindex, struct link_state *st)
{
	char buf[REQ_BUFSIZE];
	struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
	struct ifinfomsg *ifm;
	struct sockaddr_nl sa = {
		.nl_family = AF_NETLINK,
	};
	struct iovec iov;
	struct msghdr msg;
	ssize_t len;
	bool found = false;

	memset(buf, 0, sizeof(buf));
	nlh->nlmsg_len = NLMSG_LENGTH(sizeof(*ifm));
	nlh->nlmsg_type = RTM_GETLINK;
	nlh->nlmsg_flags = NLM_F_REQUEST;
	nlh->nlmsg_seq = nl_seq++;
	ifm = (struct ifinfomsg *)NLMSG_DATA(nlh);
	ifm->ifi_family = AF_UNSPEC;
	ifm->ifi_index = ifindex;

	iov.iov_base = nlh;
	iov.iov_len = nlh->nlmsg_len;
	memset(&msg, 0, sizeof(msg));
	msg.msg_name = &sa;
	msg.msg_namelen = sizeof(sa);
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;

	if (sendmsg(nl_fd, &msg, 0) < 0)
		return -errno;

	for (;;) {
		char rbuf[NL_BUFSIZE];
		struct nlmsghdr *rh;

		len = recv(nl_fd, rbuf, sizeof(rbuf), 0);
		if (len < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}

		for (rh = (struct nlmsghdr *)rbuf; NLMSG_OK(rh, len);
		     rh = NLMSG_NEXT(rh, len)) {
			if (rh->nlmsg_type == NLMSG_ERROR) {
				struct nlmsgerr *err = NLMSG_DATA(rh);

				if (err->error == 0)
					continue;
				print_extack(err, rh->nlmsg_len -
						  NLMSG_LENGTH(sizeof(*err)));
				return err->error;
			}
			if (rh->nlmsg_type != RTM_NEWLINK)
				continue;
			parse_link_msg(rh, st);
			found = true;
		}
		if (found)
			return 0;
	}
}

static int get_link_named(const char *name, struct link_state *st)
{
	unsigned int ifindex = if_nametoindex(name);
	int err;

	if (!ifindex) {
		fprintf(stderr, "etherip6ctl: %s: %s\n", name, strerror(ENODEV));
		return -ENODEV;
	}

	err = get_link_by_index((int)ifindex, st);
	if (err < 0)
		fprintf(stderr, "etherip6ctl: cannot get %s: %s\n",
			name, strerror(-err));
	return err;
}

static int show_links(const char *name)
{
	char buf[REQ_BUFSIZE];
	struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
	struct ifinfomsg *ifm;
	struct sockaddr_nl sa = {
		.nl_family = AF_NETLINK,
	};
	struct iovec iov;
	struct msghdr msg;
	bool shown = false;
	unsigned int seq;

	if (name) {
		struct link_state st;
		int err;

		err = get_link_named(name, &st);
		if (err < 0)
			return err;
		if (!st.is_etherip6) {
			fprintf(stderr, "etherip6ctl: %s is not an %s device\n",
				name, ETHERIP6_KIND);
			return -EINVAL;
		}
		print_link(&st);
		return 0;
	}

	memset(buf, 0, sizeof(buf));
	nlh->nlmsg_len = NLMSG_LENGTH(sizeof(*ifm));
	nlh->nlmsg_type = RTM_GETLINK;
	nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
	nlh->nlmsg_seq = seq = nl_seq++;
	ifm = (struct ifinfomsg *)NLMSG_DATA(nlh);
	ifm->ifi_family = AF_UNSPEC;

	iov.iov_base = nlh;
	iov.iov_len = nlh->nlmsg_len;
	memset(&msg, 0, sizeof(msg));
	msg.msg_name = &sa;
	msg.msg_namelen = sizeof(sa);
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;

	if (sendmsg(nl_fd, &msg, 0) < 0) {
		perror("sendmsg(netlink)");
		return -errno;
	}

	for (;;) {
		char rbuf[NL_BUFSIZE];
		ssize_t len = recv(nl_fd, rbuf, sizeof(rbuf), 0);
		struct nlmsghdr *rh;

		if (len < 0) {
			if (errno == EINTR)
				continue;
			perror("recv(netlink)");
			return -errno;
		}

		for (rh = (struct nlmsghdr *)rbuf; NLMSG_OK(rh, len);
		     rh = NLMSG_NEXT(rh, len)) {
			if (rh->nlmsg_type == NLMSG_DONE)
				return 0;
			if (rh->nlmsg_type == NLMSG_ERROR) {
				struct nlmsgerr *err = NLMSG_DATA(rh);

				print_extack(err, rh->nlmsg_len -
						  NLMSG_LENGTH(sizeof(*err)));
				return err->error ? err->error : -EIO;
			}
			if (rh->nlmsg_type != RTM_NEWLINK)
				continue;
			if (rh->nlmsg_seq != seq)
				continue;

			{
				struct link_state st;

				parse_link_msg(rh, &st);
				if (!st.is_etherip6)
					continue;
				if (shown)
					printf("\n");
				print_link(&st);
				shown = true;
			}
		}
	}
}

static void add_linkinfo(struct nlmsghdr *nlh, int maxlen, const struct opts *o,
			 bool for_create)
{
	struct rtattr *linkinfo, *data;
	bool has_data = o->has_remote || o->has_local || o->has_link ||
			o->has_hop || o->has_tclass || o->has_flow ||
			o->has_outer_mtu;

	if (!for_create && !has_data)
		return;

	linkinfo = addattr_nest(nlh, maxlen, IFLA_LINKINFO);
	addattr_l(nlh, maxlen, IFLA_INFO_KIND, ETHERIP6_KIND,
		  strlen(ETHERIP6_KIND) + 1);

	data = addattr_nest(nlh, maxlen, IFLA_INFO_DATA);
	if (o->has_remote)
		addattr_l(nlh, maxlen, IFLA_ETHERIP6_REMOTE, &o->remote,
			  sizeof(o->remote));
	if (o->has_local)
		addattr_l(nlh, maxlen, IFLA_ETHERIP6_LOCAL, &o->local,
			  sizeof(o->local));
	if (o->has_link)
		add_uint32(nlh, maxlen, IFLA_ETHERIP6_LINK, o->link);
	if (o->has_hop)
		add_uint8(nlh, maxlen, IFLA_ETHERIP6_HOP_LIMIT, o->hop);
	if (o->has_tclass)
		add_uint8(nlh, maxlen, IFLA_ETHERIP6_TCLASS, o->tclass);
	if (o->has_flow)
		add_uint32(nlh, maxlen, IFLA_ETHERIP6_FLOWLABEL, o->flow);
	if (o->has_outer_mtu)
		add_uint32(nlh, maxlen, IFLA_ETHERIP6_OUTER_MTU,
			   o->outer_mtu);
	addattr_nest_end(nlh, data);
	addattr_nest_end(nlh, linkinfo);
}

static int cmd_create(const struct opts *o)
{
	char buf[REQ_BUFSIZE];
	struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
	struct ifinfomsg *ifm;

	memset(buf, 0, sizeof(buf));
	nlh->nlmsg_len = NLMSG_LENGTH(sizeof(*ifm));
	nlh->nlmsg_type = RTM_NEWLINK;
	nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE |
			   NLM_F_EXCL;
	ifm = (struct ifinfomsg *)NLMSG_DATA(nlh);
	ifm->ifi_family = AF_UNSPEC;

	if (o->updown > 0) {
		ifm->ifi_flags = IFF_UP;
		ifm->ifi_change = IFF_UP;
	}

	addattr_l(nlh, sizeof(buf), IFLA_IFNAME, o->name,
		  strlen(o->name) + 1);
	if (o->has_mtu)
		add_uint32(nlh, sizeof(buf), IFLA_MTU, o->mtu);

	add_linkinfo(nlh, sizeof(buf), o, true);

	return nl_talk(nlh);
}

static int cmd_set(const struct opts *o)
{
	char buf[REQ_BUFSIZE];
	struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
	struct ifinfomsg *ifm;
	unsigned int ifindex;
	bool has_type_data;

	ifindex = if_nametoindex(o->name);
	if (!ifindex) {
		fprintf(stderr, "etherip6ctl: %s: %s\n", o->name,
			strerror(ENODEV));
		return -ENODEV;
	}

	has_type_data = o->has_remote || o->has_local || o->has_link ||
			o->has_hop || o->has_tclass || o->has_flow ||
			o->has_outer_mtu;

	if (!has_type_data && !o->has_mtu && o->updown == 0) {
		fprintf(stderr, "etherip6ctl: nothing to change\n");
		return -EINVAL;
	}

	memset(buf, 0, sizeof(buf));
	nlh->nlmsg_len = NLMSG_LENGTH(sizeof(*ifm));
	nlh->nlmsg_type = RTM_NEWLINK;
	nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
	ifm = (struct ifinfomsg *)NLMSG_DATA(nlh);
	ifm->ifi_family = AF_UNSPEC;
	ifm->ifi_index = (int)ifindex;

	if (o->updown > 0) {
		ifm->ifi_flags = IFF_UP;
		ifm->ifi_change = IFF_UP;
	} else if (o->updown < 0) {
		ifm->ifi_flags = 0;
		ifm->ifi_change = IFF_UP;
	}

	if (o->has_mtu)
		add_uint32(nlh, sizeof(buf), IFLA_MTU, o->mtu);

	add_linkinfo(nlh, sizeof(buf), o, false);

	return nl_talk(nlh);
}

static int cmd_del(const char *name)
{
	char buf[REQ_BUFSIZE];
	struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
	struct ifinfomsg *ifm;
	struct link_state st;
	int err;

	err = get_link_named(name, &st);
	if (err < 0)
		return err;

	if (!st.is_etherip6) {
		fprintf(stderr, "etherip6ctl: %s is not an %s device\n",
			name, ETHERIP6_KIND);
		return -EINVAL;
	}

	memset(buf, 0, sizeof(buf));
	nlh->nlmsg_len = NLMSG_LENGTH(sizeof(*ifm));
	nlh->nlmsg_type = RTM_DELLINK;
	nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
	ifm = (struct ifinfomsg *)NLMSG_DATA(nlh);
	ifm->ifi_family = AF_UNSPEC;
	ifm->ifi_index = st.ifindex;

	return nl_talk(nlh);
}

int main(int argc, char **argv)
{
	struct opts o;
	const char *cmd;

	if (argc < 2) {
		usage(stderr);
		return 1;
	}

	cmd = argv[1];

	if (!strcmp(cmd, "help") || !strcmp(cmd, "-h") ||
	    !strcmp(cmd, "--help")) {
		usage(stdout);
		return 0;
	}

	if (!strcmp(cmd, "show")) {
		nl_open();
		return show_links(argc > 2 ? argv[2] : NULL) ? 1 : 0;
	}

	if (!strcmp(cmd, "del")) {
		if (argc != 3) {
			usage(stderr);
			return 1;
		}
		nl_open();
		return cmd_del(argv[2]) ? 1 : 0;
	}

	if (strcmp(cmd, "create") && strcmp(cmd, "set")) {
		usage(stderr);
		return 1;
	}

	if (parse_opts(argc - 1, argv + 1, &o) < 0) {
		fprintf(stderr, "etherip6ctl: invalid arguments\n");
		usage(stderr);
		return 1;
	}

	nl_open();

	if (!strcmp(cmd, "create"))
		return cmd_create(&o) ? 1 : 0;

	return cmd_set(&o) ? 1 : 0;
}
