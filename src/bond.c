// SPDX-License-Identifier: GPL-3.0-or-later

/*
 *      B1B - Bonding mode 1 bridge helper
 *
 *      bond.c - Bond stuff
 *
 *      Copyright 2024 Ian Pilcher <arequipeno@gmail.com>
 */


#include "b1b.h"

#include <string.h>

#include <linux/rtnetlink.h>

enum b1b_bs_check_type {
	B1B_BS_CHECK_DONE,  /* have all expected attributes been parsed? */
	B1B_BS_CHECK_AUTO,  /* checking an interface that was auto-detected */
	B1B_BS_CHECK_CLI    /* checking an interface from the command line */
};

__attribute__((format(printf, 4, 5)))
static void b1b_check_log(const char *restrict const file, const int line,
			  const enum b1b_bs_check_type type,
			  const char *restrict const format, ...)
{
	va_list ap;

	if (type == B1B_BS_CHECK_DONE)
		return;

	va_start(ap, format);

	if (type == B1B_BS_CHECK_AUTO) {
		b1b_vlog(file, line, LOG_DEBUG, format, ap);
	}
	else if (type == B1B_BS_CHECK_CLI) {
		b1b_vlog(file, line, LOG_CRIT, format, ap);
		exit(1);
	}
	else {
		B1B_ABORT("Invalid type value: %d", (int)type);
	}
}

#define B1B_CHECK_LOG(type, fmt, ...)	\
		b1b_check_log(__FILE__, __LINE__, type, fmt, ##__VA_ARGS__)


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
		}

		if (bs->brname != NULL)
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

static _Bool b1b_get_bridge_info(struct b1b_global_session *const gs,
				 struct b1b_bond_session *const bs,
				 const enum b1b_bs_check_type type)
{
	int result;

	result = b1b_getlink(gs, NULL, bs->brindex, b1b_br_msg_cb, bs);
	if (result <= MNL_CB_ERROR)
		B1B_FATAL("Failed to get master info for bond: %s", bs->ifname);

	if (bs->brname == NULL) {
		B1B_CHECK_LOG(type,
			      "Failed to get master name for bond: %s",
			      bs->ifname);
		return 0;
	}

	if (bs->brtype == B1B_BR_TYPE_LINUX) {
		bs->getfdb = b1b_br_get_fdb;
		return 1;
	}
	else if (bs->brtype == B1B_BR_TYPE_OVS) {
		b1b_get_ovs_info(gs, bs);
		return 1;
	}
	else if (bs->brtype == B1B_BR_TYPE_NONE) {
		B1B_CHECK_LOG(type, "Bond master (%s) type not set: %s",
			      bs->brname, bs->ifname);
		return 0;
	}
	else if (bs->brtype == B1B_BR_TYPE_OTHER) {
		B1B_CHECK_LOG(type,
			      "Bond master (%s) not a Linux or OVS bridge: %s",
			      bs->brname, bs->ifname);
		return 0;
	}

	else {
		B1B_ABORT("Invalid bridge type value: %d", (int)type);
	}
}


/*
 *
 *	RTM_NEWLINK message parsing
 *
 */

/*
 * Check a bond session for complete information or any errors, during or
 * immediately RTM_NEWLINK message parsing (i.e. before bridge information is
 * checked).
 */
static _Bool b1b_check_bs(const struct b1b_bond_session *const bs,
			  const enum b1b_bs_check_type type)
{
	if (bs->iftype == B1B_IF_TYPE_NONE) {
		B1B_CHECK_LOG(type, "Interface type not set: %s", bs->ifname);
		return 0;
	}

	if (bs->iftype == B1B_IF_TYPE_OTHER) {
		B1B_CHECK_LOG(type, "Invalid interface type: %s", bs->ifname);
		return 0;
	}

	if (bs->mode == 0xff) {
		B1B_CHECK_LOG(type, "Interface bonding mode not set: %s",
			      bs->ifname);
		return 0;
	}

	if (bs->mode != 1) {
		B1B_CHECK_LOG(type, "Invalid bonding mode (%" PRIu8 "): %s",
			      bs->mode, bs->ifname);
		return 0;
	}

	if (bs->brindex == 0) {
		B1B_CHECK_LOG(type, "Interface master not set: %s", bs->ifname);
		return 0;
	}

	return 1;
}

/*
 * Callback for parsing (doubly) nested attributes in IFLA_INFO_DATA (only if
 * IFLA_INFO_KIND is "bond")
 */
static int b1b_bs_ld_cb(const struct nlattr *const attr, void *const data)
{
	struct b1b_bond_session *const bs = data;

	if (attr->nla_type == IFLA_BOND_MODE) {

		bs->mode = mnl_attr_get_u8(attr);
		if (b1b_check_bs(bs, B1B_BS_CHECK_DONE))
			return MNL_CB_STOP;
	}

	return MNL_CB_OK;
}
/*
 * Callback for parsing nested attributes in IFLA_LINKINFO
 */
static int b1b_bs_linkinfo_cb(const struct nlattr *const attr, void *const data)
{
	struct b1b_bond_session *const bs = data;
	const char *type_err;

	if (attr->nla_type == IFLA_INFO_KIND) {

		if (strcmp(mnl_attr_get_str(attr), "bond") == 0) {
			bs->iftype = B1B_IF_TYPE_BOND;
			return MNL_CB_OK;
		}
		else {
			bs->iftype = B1B_IF_TYPE_OTHER;
			return MNL_CB_STOP;
		}
	}

	if (attr->nla_type == IFLA_INFO_DATA) {

		if (bs->iftype == B1B_IF_TYPE_BOND) {
			return mnl_attr_parse_nested(attr, b1b_bs_ld_cb, bs);
		}
		else if (bs->iftype == B1B_IF_TYPE_NONE) {
			/*
			 * This will only happen if the IFLA_INFO_DATA attribute
			 * is before the IFLA_INFO_KIND attribute in the
			 * RTM_NEWLINK message, which shouldn't happen.
			 */
			type_err = "Interface type not set";
		}
		else if (bs->iftype == B1B_IF_TYPE_OTHER) {
			/*
			 * This shouldn't happen, because we return
			 * MNL_CB_STOP when the type is B1B_IF_TYPE_OTHER.
			 */
			type_err = "Invalid interface type";
		}
		else {
			B1B_ABORT("Invalid interface type value: %d",
				  (int)bs->iftype);
		}

		B1B_ABORT("Cannot parse interface data: %s: %s",
			  type_err, bs->ifname);
	}

	return 	MNL_CB_OK;
}

/*
 * Callback for parsing top level attributes in RTM_NEWLINK messages
 */
static int b1b_bs_attr_cb(const struct nlattr *const attr, void *const data)
{
	struct b1b_bond_session *const bs = data;

	if (attr->nla_type == IFLA_IFNAME) {
		/*
		 * Free the temporary name that was allocated in
		 * b1b_get_bond_info() or b1b_bond_msg_cb.
		 */
		free(bs->ifname);
		bs->ifname = B1B_STRDUP(mnl_attr_get_str(attr));
	}
	else if (attr->nla_type == IFLA_MASTER) {

		bs->brindex = mnl_attr_get_u32(attr);
	}
	else if (attr->nla_type == IFLA_LINKINFO) {

		return mnl_attr_parse_nested(attr, b1b_bs_linkinfo_cb, bs);
	}

	if (b1b_check_bs(bs, B1B_BS_CHECK_DONE))
		return MNL_CB_STOP;
	else
		return MNL_CB_OK;
}


/*
 *
 *	Parse information about interfaces specified on the command line
 *
 */

/*
 * Callback for parsing RTM_NEWLINK messages
 *
 * NOTE: This is also called from b1b_auto_msg_cb().
 */
static int b1b_bond_msg_cb(const struct nlmsghdr *const nlmsg, void *const data)
{
	struct b1b_bond_session *const bs = data;
	struct ifinfomsg *ifi;
	int result;

	if (nlmsg->nlmsg_type != RTM_NEWLINK)
		return MNL_CB_OK;

	B1B_ASSERT(nlmsg->nlmsg_len >= MNL_NLMSG_HDRLEN + sizeof *ifi);
	ifi = mnl_nlmsg_get_payload(nlmsg);

	bs->ifindex = ifi->ifi_index;
	bs->mode = 0xff;

	/*
	 * If we're auto-detecting interfaces, we don't know this interface's
	 * name yet, only its index.  Create a temporary name, so that we have
	 * something to use in log messages.
	 *
	 * This will be freed when the IFLA_IFNAME attribute is parsed in
	 * b1b_bs_attr_cb().
	 */
	if (bs->ifname == NULL) {
		B1B_ASPRINTF(&bs->ifname, "(index %" PRId32 ")",
			     ifi->ifi_index);
	}

	result = mnl_attr_parse(nlmsg, MNL_ALIGN(sizeof *ifi),
				b1b_bs_attr_cb, bs);
	if (result <= MNL_CB_ERROR)
		return MNL_CB_ERROR;

	return MNL_CB_OK;
}

/*
 * Get info about a specific bond, by name
 */
static void b1b_get_bond_info(struct b1b_global_session *const gs,
			      const char *restrict const name,
			      struct b1b_bond_session *const bs)
{
	int result;

	/*
	 * Make a temporary copy of the interface name, for use in logging
	 * before the IFLA_IFNAME attribute is parsed.  (See b1b_bond_msg_cb()
	 * and b1b_bs_attr_cb().)
	 */
	bs->ifname = B1B_STRDUP(name);

	result = b1b_getlink(gs, name, 0, b1b_bond_msg_cb, bs);
	if (result <= MNL_CB_ERROR)
		B1B_FATAL("Failed to get interface info: %s", name);

	if (strcmp(name, bs->ifname) != 0) {
		B1B_FATAL("Got interface into with wrong name: %s: %s:",
			  name, bs->ifname);
	}

	b1b_check_bs(bs, B1B_BS_CHECK_CLI);
}

/*
 * Get information about bonds listed on the command line
 */
void b1b_parse_bonds(struct b1b_global_session *const gs, const int argc,
		     char **const argv, int bindex)
{
	unsigned int i;

	B1B_ASSERT(bindex < argc);
	gs->bcount = argc - bindex;
	gs->bonds = B1B_ZALLOC(gs->bcount * sizeof *gs->bonds);

	for (i = 0; i < gs->bcount; ++i, ++bindex) {
		B1B_DEBUG("Getting info for bond: %s", argv[bindex]);
		//gs->bonds[i].ifname = B1B_STRDUP(argv[bindex]);
		b1b_get_bond_info(gs, argv[bindex], &gs->bonds[i]);
		if (!b1b_get_bridge_info(gs, &gs->bonds[i], B1B_BS_CHECK_CLI)) {
			B1B_FATAL("Unable to identify bond master type: %s",
				  gs->bonds[i].brname);
		}
	}


	qsort(gs->bonds, gs->bcount, sizeof *gs->bonds, b1b_bs_ifindex_cmp);
}


/*
 *
 *	Auto-detect interfaces
 *
 */

/*
 * RTM_NEWLINK message callback for auto-detect case.
 */
static int b1b_auto_msg_cb(const struct nlmsghdr *const nlmsg, void *const data)
{
	struct b1b_global_session *const gs = data;
	struct b1b_bond_session *bs;
	int result;

	bs = B1B_ZALLOC(sizeof *bs);

	result = b1b_bond_msg_cb(nlmsg, bs);
	if (result <= MNL_CB_ERROR)
		return MNL_CB_ERROR;

	if (b1b_check_bs(bs, B1B_BS_CHECK_AUTO)) {
		B1B_DEBUG("Detected mode 1 bond with master: %s", bs->ifname);
		bs->next = gs->bonds;
		gs->bonds = bs;
	}
	else {
		B1B_DEBUG("Ignoring interface: %s", bs->ifname);
		free(bs->ifname);
		free(bs);
	}

	return MNL_CB_OK;
}

/*
 * Get information about all appropriate interfaces
 */
void b1b_detect_bonds(struct b1b_global_session *const gs)
{
	struct b1b_bond_session *bs, *list, *next;
	unsigned int i;

	mnl_nlmsg_put_header(gs->buf);
	gs->nlmsg.nlmsg_type = RTM_GETLINK;
	gs->nlmsg.nlmsg_flags = NLM_F_DUMP;
	mnl_nlmsg_put_extra_header(&gs->nlmsg, sizeof(struct ifinfomsg));

	/* Builds a linked list of all mode 1 bonds that have a master */
	if (b1b_nlmsg_req(gs, b1b_auto_msg_cb, gs) <= MNL_CB_ERROR)
		B1B_FATAL("Error while auto-detecting bonds");

	list = gs->bonds;

	for (bs = list; bs != NULL; bs = bs->next) {
		bs->on_bridge = b1b_get_bridge_info(gs, bs, B1B_BS_CHECK_AUTO);
		gs->bcount += bs->on_bridge;
	}

	if (gs->bcount == 0)
		B1B_FATAL("No usable bonds detected");

	gs->bonds = B1B_ZALLOC(gs->bcount * sizeof *gs->bonds);

	for (i = 0, bs = list; bs != NULL; bs = bs->next) {
		if (bs->on_bridge)
			gs->bonds[i++] = *bs;
	}

	for (bs = list; bs != NULL; bs = next) {
		next = bs->next;
		free(bs);
	}

	qsort(gs->bonds, gs->bcount, sizeof *gs->bonds, b1b_bs_ifindex_cmp);
}
