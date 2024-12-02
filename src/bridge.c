// SPDX-License-Identifier: GPL-3.0-or-later

/*
 *	B1B - Bonding mode 1 bridge helper
 *
 *	bridge.c - Bridge stuff
 *
 *	Copyright 2024 Ian Pilcher <arequipeno@gmail.com>
 */


#include "b1b.h"

#include <string.h>

#include <linux/rtnetlink.h>


/*
 *
 *	Get the forwarding database of a Linux bridge
 *
 */

static int b1b_br_fdb_attr_cb(const struct nlattr *const attr, void *const data)
{
	union b1b_fdb_dst *const dst = data;

	if (attr->nla_type == NDA_LLADDR) {
		memcpy(dst->dst.mac, mnl_attr_get_payload(attr),
		       sizeof dst->dst.mac);
	}
	else if (attr->nla_type == NDA_VLAN) {
		dst->dst.vlan = mnl_attr_get_u16(attr);
	}

	return MNL_CB_OK;
}

static int b1b_br_fdb_msg_cb(const struct nlmsghdr *const nlmsg,
			     void *const data)
{
	static const union b1b_fdb_dst mac_mask = {
		.dst = {
			.vlan = 0,
			.mac = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff }
		}
	};

	struct b1b_bond_session *const bs = data;
	const struct ndmsg *ndm;
	union b1b_fdb_dst dst;
	int result;

	if (nlmsg->nlmsg_type == NLMSG_DONE)
		return MNL_CB_STOP;

	if (nlmsg->nlmsg_type != RTM_NEWNEIGH)
		return MNL_CB_OK;

	B1B_ASSERT(nlmsg->nlmsg_len >= MNL_NLMSG_HDRLEN + sizeof *ndm);
	ndm = mnl_nlmsg_get_payload(nlmsg);

	if (ndm->ndm_ifindex == bs->ifindex || (ndm->ndm_state & NUD_PERMANENT))
		return MNL_CB_OK;

	dst.u64 = 0;

	result = mnl_attr_parse(nlmsg, MNL_ALIGN(sizeof *ndm),
				b1b_br_fdb_attr_cb, &dst);
	if (result < 0)
		return MNL_CB_ERROR;

	if ((dst.u64 & mac_mask.u64) == 0)
		return MNL_CB_OK;

	b1b_fdb_add(bs, dst);

	return MNL_CB_OK;
}

void b1b_br_get_fdb(struct b1b_global_session *const gs,
		    struct b1b_bond_session *const bs)
{
	struct ndmsg *ndm;

	mnl_nlmsg_put_header(gs->buf);
	gs->nlmsg.nlmsg_type = RTM_GETNEIGH;
	gs->nlmsg.nlmsg_flags = NLM_F_DUMP;
	ndm = mnl_nlmsg_put_extra_header(&gs->nlmsg, sizeof *ndm);
	ndm->ndm_family = AF_BRIDGE;
	mnl_attr_put_u32(&gs->nlmsg, NDA_MASTER, bs->brindex);

	if (b1b_nlmsg_req(gs, b1b_br_fdb_msg_cb, bs) < 0) {
		B1B_FATAL("Failed to get forwarding table for bridge: %s",
			  bs->brname);
	}
}


#if 0
/*
 *
 *	Get basic information about a (Linux or OVS) bridge
 *
 */

static int b1b_br_linkinfo_cb(const struct nlattr *const attr, void *const data)
{
	struct b1b_bond_session *const bs = data;
	const char *kind;

	if (attr->nla_type == IFLA_INFO_KIND) {

		kind = mnl_attr_get_str(attr);

		if (strcmp(kind, "openvswitch") == 0) {
			bs->brtype = B1B_BR_TYPE_OVS;
		}
		else if (strcmp(kind, "bridge") == 0) {
			bs->brtype = B1B_BR_TYPE_LINUX;
		}
		else {
			bs->brtype = B1B_BR_TYPE_OTHER;
#if 0
			B1B_ERR("Invalid type of bond master: %s", kind);
			return MNL_CB_ERROR;
#endif
		}

		if (bs->brname[0] != 0)
			return MNL_CB_STOP;
	}

	return MNL_CB_OK;
}

static int b1b_br_attr_cb(const struct nlattr *const attr, void *const data)
{
	struct b1b_bond_session *const bs = data;

	if (attr->nla_type == IFLA_IFNAME) {
		bs->brname = B1B_STRDUP(mnl_attr_get_str(attr));
		if (bs->brtype != B1B_BR_TYPE_NONE)
			return MNL_CB_STOP;
	}
	else if (attr->nla_type == IFLA_LINKINFO) {
		return mnl_attr_parse_nested(attr, b1b_br_linkinfo_cb, data);
	}

	return MNL_CB_OK;
}

static int b1b_br_msg_cb(const struct nlmsghdr *const nlmsg, void *const data)
{
	struct b1b_bond_session *const bs = data;
	struct ifinfomsg *ifi;
	int result;

	if (nlmsg->nlmsg_type != RTM_NEWLINK)
		return MNL_CB_OK;

	B1B_ASSERT(nlmsg->nlmsg_len >= MNL_NLMSG_HDRLEN + sizeof *ifi);
	ifi = mnl_nlmsg_get_payload(nlmsg);

	if (ifi->ifi_index != bs->brindex)
		return MNL_CB_OK;

	bs->brtype = B1B_BR_TYPE_NONE;
	bs->brname = NULL;

	result = mnl_attr_parse(nlmsg, MNL_ALIGN(sizeof *ifi),
				b1b_br_attr_cb, bs);
	if (result <= MNL_CB_ERROR)
		return MNL_CB_ERROR;

	return MNL_CB_STOP;
}

_Bool b1b_get_bridge_info(struct b1b_global_session *const gs,
			  struct b1b_bond_session *const bs)
{
	int result;

	result = b1b_getlink(gs, NULL, bs->brindex, b1b_br_msg_cb, bs);
	if (result <= MNL_CB_ERROR)
		B1B_FATAL("Failed to get master info for bond: %s", bs->ifname);

	if (bs->brname == NULL)
		B1B_FATAL("Failed to get master name for bond: %s", bs->ifname);

	if (bs->brtype == B1B_BR_TYPE_LINUX) {
		bs->getfdb = b1b_br_get_fdb;
	}
	else if (bs->brtype == B1B_BR_TYPE_OVS) {
		b1b_get_ovs_info(gs, bs);
	}
	else {
		return 0;
#if 0
		B1B_FATAL("Unable to identify type of bond master: %s",
			  bs->brname);
#endif
	}

	return 1;
}
#endif
