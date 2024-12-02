// SPDX-License-Identifier: GPL-3.0-or-later

/*
 *	B1B - Bonding mode 1 bridge helper
 *
 *	netlink.c - netlink stuff
 *
 *	Copyright 2024 Ian Pilcher <arequipeno@gmail.com>
 */


#include "b1b.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>

#include <linux/rtnetlink.h>

#include <libmnl/libmnl.h>


/*
 *
 *	Set up a netlink (libmnl) socket
 *
 */

static struct mnl_socket * b1b_nl_open(int optname, unsigned int optval)
{
	struct mnl_socket *nlsock;
	int result;

	if ((nlsock = mnl_socket_open(NETLINK_ROUTE)) == NULL)
		B1B_FATAL("Failed to create netlink socket: %m");

	if (mnl_socket_bind(nlsock, 0, MNL_SOCKET_AUTOPID) != 0)
		B1B_FATAL("Failed to bind netlink socket: %m");

	result = mnl_socket_setsockopt(nlsock, optname, &optval, sizeof optval);
	if (result < 0)
		B1B_FATAL("Failed to set netlink socket option: %m");

	return nlsock;
}

void b1b_nlsock_open(struct b1b_global_session *const gs)
{
	gs->nlsock = b1b_nl_open(NETLINK_GET_STRICT_CHK, 1);
}

void b1b_mcsock_open(struct b1b_global_session *const gs)
{
	int fd, flags;

	gs->mcsock = b1b_nl_open(NETLINK_ADD_MEMBERSHIP, RTNLGRP_LINK);

	fd = mnl_socket_get_fd(gs->mcsock);

	flags = fcntl(fd, F_GETFL);

	if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
		B1B_FATAL("Failed to make netlink socket non-blocking: %m");
}


/*
 *
 *	libmnl socket request/response helper
 *
 */

struct b1b_cb_wrapper_data {
	mnl_cb_t msg_cb;
	void *data;
};

static int b1b_nlmsg_req_cb_wrapper(const struct nlmsghdr *const nlmsg,
				    void *const data)
{
	const struct b1b_cb_wrapper_data *const wd = data;
	int result;

	if ((result = wd->msg_cb(nlmsg, wd->data)) <= MNL_CB_STOP)
		return result;

	if (nlmsg->nlmsg_type == NLMSG_DONE)
		return MNL_CB_STOP;

	if (nlmsg->nlmsg_flags & NLM_F_MULTI)
		return MNL_CB_OK;
	else
		return MNL_CB_STOP;
}

int b1b_nlmsg_req(struct b1b_global_session *const gs, const mnl_cb_t msg_cb,
		  void *const data)
{
	static unsigned int seq;

	int result;
	ssize_t bytes;
	struct b1b_cb_wrapper_data wd;

	gs->nlmsg.nlmsg_flags |= NLM_F_REQUEST;
	gs->nlmsg.nlmsg_seq = ++seq;

	result = mnl_socket_sendto(gs->nlsock, gs->buf, gs->nlmsg.nlmsg_len);
	if (result < 0) {
		B1B_ERR("Failed to send netlink message: %m");
		return MNL_CB_ERROR;
	}

	do {
		bytes = mnl_socket_recvfrom(gs->nlsock, gs->buf, gs->bufsize);
		if (bytes < 0) {
			B1B_ERR("Failed to receive netlink message: %m");
			return MNL_CB_ERROR;
		}

		wd.msg_cb = msg_cb;
		wd.data = data;

		errno = 0;
		result = mnl_cb_run(gs->buf, bytes, seq,
				    mnl_socket_get_portid(gs->nlsock),
				    b1b_nlmsg_req_cb_wrapper, &wd);

	} while (result >= MNL_CB_OK);

	if (result <= MNL_CB_ERROR) {
		if (errno == 0)
			B1B_ERR("Error parsing netlink message");
		else
			B1B_ERR("Netlink error: %m");
	}

	return result;
}


/*
 *
 *	Get information about a network interface by name or index
 *
 */

int b1b_getlink(struct b1b_global_session *const gs,
		const char *restrict const ifname, const int32_t ifindex,
		const mnl_cb_t msg_cb, void *const data)
{
	struct ifinfomsg *ifi;

	mnl_nlmsg_put_header(gs->buf);
	gs->nlmsg.nlmsg_type = RTM_GETLINK;
	ifi = mnl_nlmsg_put_extra_header(&gs->nlmsg, sizeof *ifi);
	ifi->ifi_index = ifindex;

	if (ifname != NULL)
		mnl_attr_put_strz(&gs->nlmsg, IFLA_IFNAME, ifname);

	return b1b_nlmsg_req(gs, msg_cb, data);
}


/*
 *
 *	Process netlink multicast messages
 *
 */

int b1b_bs_ifindex_cmp(const void *const e1, const void *const e2)
{
	const struct b1b_bond_session *const bs1 = e1;
	const struct b1b_bond_session *const bs2 = e2;

	if (bs1->ifindex < bs2->ifindex)
		return -1;

	if (bs1->ifindex > bs2->ifindex)
		return 1;

	return 0;
}

static int b1b_mc_attr_cb(const struct nlattr *const attr, void *const data)
{
	struct b1b_bond_session *const bs = data;

	if (attr->nla_type == IFLA_EVENT) {

		if (mnl_attr_get_u32(attr) == IFLA_EVENT_BONDING_FAILOVER) {

			if (bs->failover_event) {
				B1B_DEBUG("Duplicate failover event: %s",
					  bs->ifname);
			}
			else {
				bs->failover_event = 1;
			}
		}

		return MNL_CB_STOP;
	}

	return MNL_CB_OK;
}

static int b1b_mc_msg_cb(const struct nlmsghdr *const nlmsg,
			 void *const data)
{
	struct b1b_global_session *const gs = data;
	const struct ifinfomsg *ifi;
	struct b1b_bond_session key, *bs;
	int result;

	if (nlmsg->nlmsg_type != RTM_NEWLINK)
		return MNL_CB_OK;

	B1B_ASSERT(nlmsg->nlmsg_len >= MNL_NLMSG_HDRLEN + sizeof *ifi);
	ifi = mnl_nlmsg_get_payload(nlmsg);

	key.ifindex = ifi->ifi_index;
	bs = bsearch(&key, gs->bonds, gs->bcount, sizeof key,
		     b1b_bs_ifindex_cmp);
	if (bs == NULL)
		return MNL_CB_OK;

	result = mnl_attr_parse(nlmsg, MNL_ALIGN(sizeof *ifi),
				b1b_mc_attr_cb, bs);
	if (result <= MNL_CB_ERROR)
		return MNL_CB_ERROR;

	return MNL_CB_OK;
}

void b1b_mcast_process(struct b1b_global_session *const gs)
{
	ssize_t bytes;
	unsigned int i;
	int result;
	_Bool parse_error;

	for (i = 0; i < gs->bcount; ++i)
		gs->bonds[i].failover_event = 0;

	i = mnl_socket_get_portid(gs->mcsock);
	parse_error = 0;

	while (1) {

		bytes = mnl_socket_recvfrom(gs->mcsock, gs->buf, gs->bufsize);
		if (bytes < 0) {
			if (errno == EAGAIN)
				break;
			B1B_FATAL("Failed to receive netlink message: %m");
		}

		errno = 0;
		result = mnl_cb_run(gs->buf, bytes, 0, i, b1b_mc_msg_cb, gs);
		if (result <= MNL_CB_ERROR && !parse_error) {
			parse_error = 1;
			if (errno == 0)
				B1B_ERR("Failed to parse netlink message(s)");
			else
				B1B_ERR("Netlink error: %m");
		}
	}

	for (i = 0; i < gs->bcount; ++i) {
		if (gs->bonds[i].failover_event)
			b1b_send_garps(gs, gs->bonds + i);
	}
}
