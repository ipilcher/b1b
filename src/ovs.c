// SPDX-License-Identifier: GPL-3.0-or-later

/*
 *	B1B - Bonding mode 1 bridge helper
 *
 *	ovs.c - Open vSwitch stuff
 *
 *	Copyright 2024 Ian Pilcher <arequipeno@gmail.com>
 */


#include "b1b.h"

#include <inttypes.h>
#include <stdarg.h>
#include <string.h>

#include <fcntl.h>
#include <sys/un.h>

#include <linux/rtnetlink.h>

#include <json-c/json_object.h>
#include <json-c/json_tokener.h>
#include <json-c/json_util.h>


static const char b1b_ovs_pid_file[] = "/run/openvswitch/ovs-vswitchd.pid";


/*
 *
 *	Connect to ovs-vswitchd via a UNIX socket
 *
 */

static pid_t b1b_ovs_pid(void)
{
	struct flock lck = { .l_type = F_WRLCK, .l_whence = SEEK_SET };
	int pidfd;

	/*
	 * ovs-vswitchd should have its PID file locked, so just check the PID
	 * of the lock, rather than parsing the file contents.
	 */

	if ((pidfd = open(b1b_ovs_pid_file, O_RDONLY)) < 0)
		B1B_FATAL("Failed to open PID file: %s: %m", b1b_ovs_pid_file);

	if (fcntl(pidfd, F_GETLK, &lck) < 0) {
		B1B_FATAL("Failed to query PID file lock: %s: %m",
				b1b_ovs_pid_file);
	}

	if (lck.l_type == F_UNLCK)
		B1B_FATAL("PID file not locked: %s: %m",b1b_ovs_pid_file);

	if (close(pidfd) < 0)
		B1B_FATAL("Failed to close PID file: %s:%m", b1b_ovs_pid_file);

	return lck.l_pid;
}

static void b1b_ovs_open(struct b1b_global_session *const gs)
{
	struct sockaddr_un sun = { .sun_family = AF_UNIX };
	int result;

	result = asprintf(&gs->ovssock_path,
			  "/run/openvswitch/ovs-vswitchd.%" PRIdMAX ".ctl",
			  (intmax_t)b1b_ovs_pid());
	if (result < 0)
		B1B_FATAL("Failed to format UNIX socket path: %m");

	if ((unsigned int)result >= sizeof sun.sun_path)
		B1B_FATAL("UNIX socket path too long");

	memcpy(sun.sun_path, gs->ovssock_path, result + 1);

	if ((gs->ovssock = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
		B1B_FATAL("Failed to create UNIX socket: %s: %m", sun.sun_path);

	result = connect(gs->ovssock, (struct sockaddr *)&sun, sizeof sun);
	if (result < 0) {
		B1B_FATAL("Failed to connect UNIX socket: %s, %m",
			  sun.sun_path);
	}
}


/*
 *
 *	Send a JSON-RPC request to ovs-vswitchd
 *
 */

static void b1b_json_req_add(json_object *restrict const req,
			     const char *restrict const name,
			     json_object *restrict const member)
{
	static const unsigned int opts =
		JSON_C_OBJECT_ADD_KEY_IS_NEW | JSON_C_OBJECT_ADD_CONSTANT_KEY;

	if (json_object_object_add_ex(req, name, member, opts) < 0)
		B1B_FATAL("Failed to add member to JSON request: %s", name);
}

static uint64_t b1b_ovs_rpc_send(struct b1b_global_session *const gs,
				 const char *restrict const method,
				 const char *restrict const param)
{
	static uint64_t reqid;

	json_object *req, *member, *elem;
	int result;

	if ((req = json_object_new_object()) == NULL)
		B1B_FATAL("Failed to create JSON-RPC request: %m");

	if ((member = json_object_new_uint64(++reqid)) == NULL)
		B1B_FATAL("Failed to create JSON-RPC request ID: %m");

	b1b_json_req_add(req, "id", member);

	if ((member = json_object_new_string(method)) == NULL)
		B1B_FATAL("Failed to create JSON-RPC method: %m");

	b1b_json_req_add(req, "method", member);

	if ((member = json_object_new_array_ext(param != NULL)) == NULL)
		B1B_FATAL("Failed to create JSON-RPC parameter list: %m");

	if (param != NULL) {

		if ((elem = json_object_new_string(param)) == NULL)
			B1B_FATAL("Failed to create JSON-RPC parameter: %m");

		if (json_object_array_add(member, elem) < 0)
			B1B_FATAL("Failed to add JSON-RPC parameter to list");
	}

	b1b_json_req_add(req, "params", member);

	if (gs->ovssock < 0)
		b1b_ovs_open(gs);

	result = json_object_to_fd(gs->ovssock, req,
				   JSON_C_TO_STRING_NOSLASHESCAPE);
	if (result < 0) {
		B1B_FATAL("Failed to send JSON-RPC request: %s",
			  json_util_get_last_err());
	}

	if (json_object_put(req) != 1)
		B1B_FATAL("Failed to free JSON-RPC request");

	return reqid;
}


/*
 *
 *	Receive a JSON-RPC response from ovs-vswitchd
 *
 */

static json_object *b1b_json_resp_get(const json_object *restrict const resp,
				      const char *restrict const name,
				      ...)
{
	json_object *member;
	json_type type;
	va_list ap;

	if (!json_object_object_get_ex(resp, name, &member)) {
		B1B_FATAL("JSON-RPC response does not contain member: %s",
			  name);
	}

	va_start(ap, name);

	while ((type = va_arg(ap, json_type)) >= 0) {

		if (json_object_is_type(member, type))
			break;
	}

	va_end(ap);

	if (type < 0) {
		B1B_FATAL("Incorrect type of JSON-RPC response member: %s: %s",
			  name,
			  json_type_to_name(json_object_get_type(member)));
	}

	return member;
}

static _Bool b1b_ovs_rpc_recv(struct b1b_global_session *const gs,
			      const uint64_t reqid)
{
	struct json_tokener *tok;
	json_object *resp, *member;
	ssize_t bytes;
	enum json_type type;
	_Bool result;

	if ((tok = json_tokener_new_ex(2)) == NULL)
		B1B_FATAL("Failed to create JSON parser: %m");

	bytes = read(gs->ovssock, gs->buf, gs->bufsize);
	if (bytes < 0) {
		B1B_FATAL("Failed to receive JSON-RPC response: %s: %m",
			  gs->ovssock_path);
	}

	if ((size_t)bytes == gs->bufsize)
		B1B_FATAL("JSON-RPC response too large: %zd", bytes);

	gs->buf[bytes] = 0;

	resp = json_tokener_parse_ex(tok, gs->str, bytes + 1);
	if (resp == NULL) {
		B1B_FATAL("Failed to parse JSON-RPC response: %s",
			  json_tokener_error_desc(json_tokener_get_error(tok)));
	}

	json_tokener_free(tok);

	if (!json_object_is_type(resp, json_type_object))
		B1B_FATAL("JSON-RPC response is not a JSON object");

	member = b1b_json_resp_get(resp, "id", json_type_int, -1);

	if (json_object_get_uint64(member) != reqid) {
		B1B_FATAL("JSON-RPC response ID does not match request: "
				"request: %" PRIu64 ", response: %" PRIu64,
			  reqid, json_object_get_uint64(member));
	}

	member = b1b_json_resp_get(resp, "error", json_type_string,
				   json_type_null, -1);

	if ((type = json_object_get_type(member)) == json_type_string) {

		result = 0;
	}
	else {  /* must be a string (ensured by b1b_json_resp_get()) */

		member = b1b_json_resp_get(resp, "result",
					   json_type_string, -1);
		result = 1;
	}

	if ((bytes = json_object_get_string_len(member)) <= 0)
		B1B_FATAL("JSON-RPC response has zero length result/error");

	/* Buffer held JSON response, so string definitely fits */
	memcpy(gs->buf, json_object_get_string(member), bytes);
	gs->buf[bytes - 1] = 0;  /* remove newline */

	if (json_object_put(resp) != 1)
		B1B_FATAL("Failed to free JSON-RPC response");

	return result;
}


/*
 *
 *	Simple line iterator for parsing ovs-vswitchd results
 *
 */

struct b1b_line_iter {
	char *line;
	char *eol;  /* if NULL, last (possibly empty) line has been returned */
};

struct b1b_line_iter *b1b_line_iter_new(char *const buf)
{
	struct b1b_line_iter *iter;

	iter = B1B_ZALLOC(sizeof *iter);
	iter->line = buf;
	iter->eol = buf;

	return iter;
}

char *b1b_line_iter_next(struct b1b_line_iter *const iter)
{
	char *result;

	if (iter->eol == NULL)
		return NULL;

	result = iter->line;

#if 0
	/* Enable these (untested) lines to make the iterator non-destructive */
	if (iter->eol != iter->line)
		*iter->eol = '\n';
#endif

	if ((iter->eol = strchr(iter->line, '\n')) != NULL) {
		*iter->eol = 0;
		iter->line = iter->eol + 1;
	}

	return result;
}


/*
 *
 *	Get the forwarding database of an OVS bridge
 *
 */

static void b1b_ovs_get_fdb(struct b1b_global_session *const gs,
			    struct b1b_bond_session *const bs)
{
	uint64_t reqid;
	struct b1b_line_iter *iter;
	char *line;
	int result;
	uint32_t ofport;
	union b1b_fdb_dst dst;


	reqid = b1b_ovs_rpc_send(gs, "fdb/show", bs->brname);
	if (!b1b_ovs_rpc_recv(gs, reqid))
		B1B_FATAL("Error response from OVS daemon: %s", gs->str);

	iter = b1b_line_iter_new(gs->str);
	b1b_line_iter_next(iter);  /* skip header */

	while ((line = b1b_line_iter_next(iter)) != NULL) {

		if (strncmp(line, "LOCAL", sizeof("LOCAL") - 1) == 0)
			continue;

		result = sscanf(line, " %" SCNu32 " %" SCNu16
					" %" SCNx8 ":%" SCNx8 ":%" SCNx8
					":%" SCNx8 ":%" SCNx8 ":%" SCNx8,
				&ofport, &dst.dst.vlan,
				&dst.dst.mac[0], &dst.dst.mac[1],
				&dst.dst.mac[2], &dst.dst.mac[3],
				&dst.dst.mac[4], &dst.dst.mac[5]);
		if (result != 8)
			B1B_FATAL("Failed to parse result from OVS daemon");

		if (ofport != bs->ofport)
			b1b_fdb_add(bs, dst);
	}

	free(iter);
}


/*
 *
 *	Get OVS-specific bridge information
 *
 */

static int b1b_ovs_msg_cb(const struct nlmsghdr *const nlmsg, void *const data)
{
	struct b1b_bond_session *const bs = data;
	struct ifinfomsg *ifi;

	if (nlmsg->nlmsg_type != RTM_NEWLINK)
		return MNL_CB_OK;

	B1B_ASSERT(nlmsg->nlmsg_len >= MNL_NLMSG_HDRLEN + sizeof *ifi);
	ifi = mnl_nlmsg_get_payload(nlmsg);
	bs->brindex = ifi->ifi_index;

	return MNL_CB_STOP;
}

void b1b_get_ovs_info(struct b1b_global_session *const gs,
		      struct b1b_bond_session *const bs)
{
	uint64_t reqid;
	struct b1b_line_iter *iter;
	char *line, *ifname, *brname;
	uint32_t ofport;
	int result;

	reqid = b1b_ovs_rpc_send(gs, "dpif/show", NULL);
	if (!b1b_ovs_rpc_recv(gs, reqid))
		B1B_FATAL("Error response from OVS daemon: %s", gs->str);

	iter = b1b_line_iter_new(gs->str);
	b1b_line_iter_next(iter);  /* skip header */

	brname = NULL;

	while ((line = b1b_line_iter_next(iter)) != NULL) {

		result = sscanf(line, " %m[^: ] %" SCNu32, &ifname, &ofport);

		if (result == 1) {
			free(brname);
			brname = ifname;
			continue;
		}

		if (result != 2)
			B1B_FATAL("Failed to parse result from OVS daemon");

		result = strcmp(ifname, bs->ifname);
		free(ifname);

		if (result == 0)
			break;
	}

	free(iter);

	if (brname == NULL || line == NULL)
		B1B_FATAL("Failed to identify OVS bridge and port");

	/*
	 * The previously identified bond master is the OVS system device.
	 * Update the bond session with the actual bridge interface info.
	 */

	free(bs->brname);
	bs->brname = brname;
	bs->ofport = ofport;
	bs->getfdb = b1b_ovs_get_fdb;
	bs->brindex = 0;

	result = b1b_getlink(gs, bs->brname, 0, b1b_ovs_msg_cb, bs);
	if (result <= MNL_CB_ERROR)
		B1B_FATAL("Failed to get OVS bridge info: %s", bs->brname);

	if (bs->brindex == 0)
		B1B_FATAL("Failed to get OVS bridge index: %s", bs->brname);
}
