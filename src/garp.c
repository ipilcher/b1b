// SPDX-License-Identifier: GPL-3.0-or-later

/*
 *	B1B - Bonding mode 1 bridge helper
 *
 *	garp.c - send gratuitous ARP requests
 *
 *	Copyright 2024 Ian Pilcher <arequipeno@gmail.com>
 */


#include "b1b.h"

#include <string.h>

#include <netinet/ip.h>
#include <net/ethernet.h>
#include <net/if_arp.h>

#include <linux/if_packet.h>

#include <savl.h>

/* htons() equivalent macro for static initializers */
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define B1B_HTONS(s)	((uint16_t)(s))
#elif __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define B1B_HTONS(s)	__builtin_bswap16(s)
#else
#error "__BYTE_ORDER__ is not __ORDER_BIG_ENDIAN__ or __ORDER_LITTLE_ENDIAN__"
#endif


/* First 12 bytes of ARP frame */
struct b1b_eth_macs {
	uint8_t dst[6];  /* ff:ff:ff:ff:ff:ff */
	uint8_t src[6];
};
_Static_assert(sizeof(struct b1b_eth_macs) == 12, "struct b1b_eth_macs size");

/* Next 4 bytes of ARP frame - ONLY FOR 802.1Q TAGGED FRAMES */
struct b1b_vlan_hdr {
	uint16_t etype;  /* htons(ETH_P_8021Q) */
	uint16_t vid;  /* network byte order; PCP & DEI are always zero */
};
_Static_assert(sizeof(struct b1b_vlan_hdr) == 4, "struct b1b_vlan_hdr size");

/* Remaining 30 bytes of ARP frame */
struct b1b_arp {
	uint16_t etype;  /* htons(ETH_P_ARP) */
	uint16_t htype;  /* htons(ARPHRD_ETHER) */
	uint16_t ptype;  /* htons(ETH_P_IP) */
	uint8_t hlen;  /* ETH_ALEN (6) */
	uint8_t plen;  /* 4 (IPv4 address size) */
	uint16_t op;  /* htons(ARPOP_REPLY) */
	uint8_t sha[ETH_ALEN];  /* source hardware address (MAC) */
	struct in_addr spa;  /* source protocol (IPv4) address (0.0.0.0) */
	uint8_t tha[ETH_ALEN];  /* target hw addr (00:00:00:00:00:00:00) */
	struct in_addr tpa;  /* target protocol address (0.0.0.0) */
}
__attribute__((packed));
_Static_assert(sizeof(struct b1b_arp) == 30, "struct b1b_arp size");


void b1b_arpsock_open(struct b1b_global_session *const gs)
{
	if ((gs->arpsock = socket(AF_PACKET, SOCK_RAW, 0)) < 0)
		B1B_FATAL("Failed to create ARP socket: %m");
}

static void b1b_send_garp(const struct b1b_global_session *const gs,
			  const struct b1b_bond_session *const bs,
			  const struct b1b_dst dst)
{
	/* .src will be set dynamically */
	static struct b1b_eth_macs macs = {
		.dst = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff }
	};

	/* .vid will be set dynamically */
	static struct b1b_vlan_hdr vlan = { .etype = B1B_HTONS(ETH_P_8021Q) };

	/* .sha will be set dynamically */
	static struct b1b_arp arp = {
		.etype = B1B_HTONS(ETH_P_ARP),
		.htype = B1B_HTONS(ARPHRD_ETHER),
		.ptype = B1B_HTONS(ETH_P_IP),
		.hlen = ETH_ALEN,
		.plen = sizeof(struct in_addr),
		.op = B1B_HTONS(ARPOP_REPLY),
		.spa = { .s_addr = 0 },
		.tha = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
		.tpa = { .s_addr = 0 }
	};

	/*
	 * .sll_protocol, .sll_hatype, and .sll_pkttype are not set;
	 * .sll_ifindex will be set dynamically
	 */
	static struct sockaddr_ll sll = {
		.sll_family = AF_PACKET,
		.sll_halen = ETH_ALEN,
		.sll_addr = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff }
	};

	/* [1] and (possibly) [2] will by set dynamically */
	static struct iovec iovs[3] = {
		[0] = { .iov_base = &macs, .iov_len = sizeof macs }
	};

	/*
	 * .msg_control, .msg_controllen, and .msg_flags are not set;
	 * .msg_iovlen will be set dynamically
	 */
	static struct msghdr msg = {
		.msg_name = &sll,
		.msg_namelen = sizeof sll,
		.msg_iov = iovs
	};

	memcpy(macs.src, dst.mac, sizeof dst.mac);
	memcpy(arp.sha, dst.mac, sizeof dst.mac);
	sll.sll_ifindex = bs->ifindex;

	if (dst.vlan != 0) {
		vlan.vid = dst.vlan;
		iovs[1].iov_base = &vlan;
		iovs[1].iov_len = sizeof vlan;
		iovs[2].iov_base = &arp;
		iovs[2].iov_len = sizeof arp;
		msg.msg_iovlen = 3;
	}
	else {
		iovs[1].iov_base = &arp;
		iovs[2].iov_len = sizeof arp;
		msg.msg_iovlen = 2;
	}

	if (sendmsg(gs->arpsock, &msg, 0) < 0) {
		B1B_ERR("Failed to send gratuitous ARP for"
				" %02" PRIx8 ":%02" PRIx8 ":%02" PRIx8
				":%02" PRIx8 ":%02" PRIx8 ":%02" PRIx8
				" via %s.%" PRIu16 ": %m",
			dst.mac[0], dst.mac[1], dst.mac[2], dst.mac[3],
			dst.mac[4], dst.mac[5], bs->ifname, dst.vlan);
	}

	B1B_DEBUG("Sent gratuitous ARP for"
			" %02" PRIx8 ":%02" PRIx8 ":%02" PRIx8
			":%02" PRIx8 ":%02" PRIx8 ":%02" PRIx8
			" via %s.%" PRIu16,
		dst.mac[0], dst.mac[1], dst.mac[2], dst.mac[3],
		dst.mac[4], dst.mac[5], bs->ifname, dst.vlan);
}

void b1b_send_garps(struct b1b_global_session *const gs,
		    struct b1b_bond_session *const bs)
{
	struct savl_node *n;
	struct b1b_dst_node *dn;

	B1B_DEBUG("Sending gratuitous ARP requests for %s via %s",
		  bs->brname, bs->ifname);

	bs->getfdb(gs, bs);

	for (n = savl_first(bs->fdbtree); n != NULL; n = savl_next(n)) {
		dn = SAVL_NODE_CONTAINER(n, struct b1b_dst_node, avl);
		b1b_send_garp(gs, bs, dn->dst.dst);
	}

	b1b_fdb_free(bs);
}
