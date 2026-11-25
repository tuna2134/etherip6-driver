/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * EtherIP over IPv6 (RFC 3378) - ABI shared by the kernel module and
 * the etherip6ctl userspace tool.
 *
 * Note: these rtnetlink attributes are private to this out-of-tree
 * driver.  The numbers below are not part of the upstream Linux UAPI;
 * if this driver is ever merged, the attribute numbers/names must be
 * assigned through the normal UAPI review process.
 */
#ifndef _ETHERIP6_UAPI_H
#define _ETHERIP6_UAPI_H

#include <linux/types.h>

/* rtnetlink link kind (IFLA_INFO_KIND) */
#define ETHERIP6_KIND		"etherip6"

/* RFC 3378: IP protocol 97, 2 byte EtherIP header, version 3. */
#define ETHERIP6_IPPROTO	97
#define ETHERIP6_VERSION	3
#define ETHERIP6_HLEN		2

/*
 * IFLA_INFO_DATA attributes for the "etherip6" link kind.
 *
 *   IFLA_ETHERIP6_REMOTE    : struct in6_addr, peer (tunnel far end)
 *   IFLA_ETHERIP6_LOCAL     : struct in6_addr, local tunnel source address
 *   IFLA_ETHERIP6_LINK      : __u32, ifindex of the egress device (0 = route)
 *   IFLA_ETHERIP6_HOP_LIMIT : __u8, outer IPv6 hop limit (0 = default)
 *   IFLA_ETHERIP6_TCLASS    : __u8, outer IPv6 traffic class
 *   IFLA_ETHERIP6_FLOWLABEL : __u32, outer IPv6 flow label (20 bit)
 *   IFLA_ETHERIP6_OUTER_MTU : __u32, cap for outer IPv6 packets (0 = route)
 */
enum {
	IFLA_ETHERIP6_UNSPEC,
	IFLA_ETHERIP6_REMOTE,
	IFLA_ETHERIP6_LOCAL,
	IFLA_ETHERIP6_LINK,
	IFLA_ETHERIP6_HOP_LIMIT,
	IFLA_ETHERIP6_TCLASS,
	IFLA_ETHERIP6_FLOWLABEL,
	IFLA_ETHERIP6_OUTER_MTU,
	__IFLA_ETHERIP6_MAX,
};

#define IFLA_ETHERIP6_MAX	(__IFLA_ETHERIP6_MAX - 1)

/* Defaults and limits. */
#define ETHERIP6_DEFAULT_MTU		9000
#define ETHERIP6_DEFAULT_HOP_LIMIT	64
#define ETHERIP6_MIN_MTU		68
#define ETHERIP6_MAX_FLOWLABEL		0x000fffffU
/* 40 (outer IPv6) + 2 (EtherIP) bytes of encapsulation overhead. */
#define ETHERIP6_OVERHEAD		(40 + ETHERIP6_HLEN)
#define ETHERIP6_MAX_FRAME		(65535 - ETHERIP6_OVERHEAD)
/* Leave room for one VLAN tag on top of the tunnel device. */
#define ETHERIP6_MAX_MTU		(ETHERIP6_MAX_FRAME - 18)

#endif /* _ETHERIP6_UAPI_H */
