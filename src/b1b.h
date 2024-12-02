// SPDX-License-Identifier: GPL-3.0-or-later

/*
 *	B1B - Bonding mode 1 bridge helper
 *
 *	b1b.h - Internal header
 *
 *	Copyright 2024 Ian Pilcher <arequipeno@gmail.com>
 */


#ifndef B1B_H_INCLUDED
#define B1B_H_INCLUDED

#include <stdarg.h>
#include <stdlib.h>
#include <syslog.h>

#include <net/if.h>

#include <libmnl/libmnl.h>
#include <savl.h>


struct b1b_bond_session;

enum __attribute__((packed)) b1b_br_type {
	B1B_BR_TYPE_NONE = 0,
	B1B_BR_TYPE_LINUX,
	B1B_BR_TYPE_OVS,
	B1B_BR_TYPE_OTHER  /* bond master is not a Linux or OVS bridge */
};
_Static_assert(sizeof(enum b1b_br_type) == sizeof(_Bool), "b1b_br_type size");

enum __attribute__((packed)) b1b_if_type {
	B1B_IF_TYPE_NONE = 0,
	B1B_IF_TYPE_BOND,
	B1B_IF_TYPE_OTHER
};
_Static_assert(sizeof(enum b1b_if_type) == sizeof(_Bool), "b1b_if_type size");

struct b1b_global_session {
	struct mnl_socket *nlsock;  /* request/response netlink socket */
	struct mnl_socket *mcsock;  /* multicast netlink socket */
	char *ovssock_path;
	struct b1b_bond_session *bonds;  /* sorted array or linked list */
	unsigned int bcount;  /* number of bonds */
	size_t bufsize;
	int arpsock;
	int ovssock;
	union {
		struct nlmsghdr nlmsg;
		/* Standard C doesn't allow flexible array members in unions */
		uint8_t buf[1];
		char str[1];
	};
};

struct b1b_bond_session {
	char *brname;
	char *ifname;
	void (*getfdb)(struct b1b_global_session *gs,
		       struct b1b_bond_session *bs);
	union {
		struct savl_node *fdbtree;
		struct b1b_bond_session *next;
	};
	int32_t ifindex;  /* interface index of bond */
	int32_t brindex;  /* index of bridge to which bond is attached */
	uint32_t ofport;  /* only if bond is attached to an OVS switch */
	enum b1b_br_type brtype;
	uint8_t mode;  /* must be 1 */
	union {
		enum b1b_if_type iftype;
		_Bool on_bridge;
		_Bool failover_event;
	};
};

struct b1b_dst {
	uint16_t vlan;
	uint8_t mac[6];
};
_Static_assert(sizeof(struct b1b_dst) == sizeof(uint64_t),
	       "struct b1b_dst size");

union b1b_fdb_dst {
	struct b1b_dst dst;
	uint64_t u64;
};

struct b1b_dst_node {
	struct savl_node avl;
	union b1b_fdb_dst dst;
};


/*
 *
 *	Logging
 *
 */

extern _Bool b1b_debug;

__attribute__((format(printf, 4, 0)))
void b1b_vlog(const char *restrict file, int line, int level,
	      const char *restrict format, va_list ap);
__attribute__((format(printf, 4, 5)))
void b1b_log(const char *restrict file, int line, int level,
	     const char *restrict format, ...);


#define B1B_LOG(lvl, fmt, ...)	\
		b1b_log(__FILE__, __LINE__, lvl, fmt, ##__VA_ARGS__)

#define B1B_ALERT(fmt, ...)	B1B_LOG(LOG_ALERT, fmt, ##__VA_ARGS__)
#define B1B_CRIT(fmt, ...)	B1B_LOG(LOG_CRIT, fmt, ##__VA_ARGS__)
#define B1B_ERR(fmt, ...)	B1B_LOG(LOG_ERR, fmt, ##__VA_ARGS__)
#define B1B_WARN(fmt, ...)	B1B_LOG(LOG_WARNING, fmt, ##__VA_ARGS__)
#define B1B_NOTICE(fmt, ...)	B1B_LOG(LOG_NOTICE, fmt, ##__VA_ARGS__)
#define B1B_INFO(fmt, ...)	B1B_LOG(LOG_INFO, fmt, ##__VA_ARGS__)

#define B1B_DEBUG(fmt, ...)	\
		if (b1b_debug) B1B_LOG(LOG_DEBUG, fmt, ##__VA_ARGS__)

/* Log an unexpected error and abort */
#define B1B_ABORT(...)	do { B1B_ALERT(__VA_ARGS__); abort(); } while (0)

/* Log a fatal error and exit */
#define B1B_FATAL(...)	do { B1B_CRIT(__VA_ARGS__); exit(1); } while (0)

/* Abort if expr is not true */
#define B1B_ASSERT(expr)						\
		do {							\
			if (!(expr))					\
				B1B_ABORT("Assertion failed");		\
		} while (0)


/*
 *
 *	Memory allocation
 *
 */

void *b1b_zalloc(size_t size, const char *file, int line);
char *b1b_strdup(const char *restrict s, const char *restrict file, int line);

__attribute__((format(printf, 4, 5)))
int b1b_asprintf(const char *restrict file, const int line,
		 char **restrict strp, const char *restrict fmt, ...);


#define B1B_ZALLOC(size)	b1b_zalloc(size, __FILE__, __LINE__)
#define B1B_STRDUP(s)		b1b_strdup(s, __FILE__, __LINE__)
#define B1B_ASPRINTF(sp, fmt, ...)	\
		b1b_asprintf(__FILE__, __LINE__, sp, fmt, ##__VA_ARGS__)


/*
 *	netlink.c
 */

int b1b_bs_ifindex_cmp(const void *e1, const void *e2);
void b1b_nlsock_open(struct b1b_global_session *gs);
void b1b_mcsock_open(struct b1b_global_session *gs);
int b1b_nlmsg_req(struct b1b_global_session *gs, mnl_cb_t msg_cb, void *data);
void b1b_mcast_process(struct b1b_global_session *gs);
int b1b_getlink(struct b1b_global_session *gs, const char *restrict ifname,
		int32_t ifindex, mnl_cb_t msg_cb, void *data);

/*
 *	bridge.c
 */
void b1b_br_get_fdb(struct b1b_global_session *gs, struct b1b_bond_session *bs);

/*
 *	ovs.c
 */
void b1b_get_ovs_info(struct b1b_global_session *gs,
		      struct b1b_bond_session *bs);

/*
 *	bond.c
 */
void b1b_detect_bonds(struct b1b_global_session *gs);
void b1b_parse_bonds(struct b1b_global_session *gs, const int argc,
		     char **argv, int bindex);

/*
 *	fdbtree.c
 */
void b1b_fdb_add(struct b1b_bond_session *bs, union b1b_fdb_dst dst);
void b1b_fdb_free(struct b1b_bond_session *bs);

/*
 *	garp.c
 */
void b1b_arpsock_open(struct b1b_global_session *gs);
void b1b_send_garps(struct b1b_global_session *gs, struct b1b_bond_session *bs);

#endif  /* B1B_H_INCLUDED */
