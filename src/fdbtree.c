// SPDX-License-Identifier: GPL-3.0-or-later

/*
 *	B1B - Bonding mode 1 bridge helper
 *
 *	fdbtree.c - MAC & VLAN AVL tree
 *
 *	Copyright 2024 Ian Pilcher <arequipeno@gmail.com>
 */


#include "b1b.h"


#if UINTPTR_MAX >= UINT64_MAX
#define B1B_DST_IN_KEY
#endif


static int b1b_fdb_cmp_cb(const union savl_key key,
			  const struct savl_node *const node)
{
	const struct b1b_dst_node *dn;
	uint64_t k64;

	dn = SAVL_NODE_CONTAINER(node, struct b1b_dst_node, avl);

#ifdef B1B_DST_IN_KEY
	k64 = key.u;
#else
	k64 = *(const uint64_t *)key.p;
#endif

	if (k64 < dn->dst.u64)
		return -1;

	if (k64 > dn->dst.u64)
		return 1;

	return 0;
}

static void b1b_fdb_free_cb(struct savl_node *const node)
{
	free(SAVL_NODE_CONTAINER(node, struct b1b_dst_node, avl));
}

void b1b_fdb_add(struct b1b_bond_session *const bs, const union b1b_fdb_dst dst)
{
	struct b1b_dst_node *dn;
	union savl_key key;

	dn = B1B_ZALLOC(sizeof *dn);
	dn->dst = dst;

#ifdef B1B_DST_IN_KEY
	key.u = dst.u64;
#else
	key.p = &dst.u64;
#endif

	if (savl_try_add(&bs->fdbtree, b1b_fdb_cmp_cb, key, &dn->avl) != NULL) {
		B1B_DEBUG("Duplicate destination: %s:"
				" %02" PRIx8 ":%02" PRIx8 ":%02" PRIx8
				":%02" PRIx8 ":%02" PRIx8 ":%02" PRIx8
				" (%" PRIu16 ")",
			  bs->brname, dst.dst.mac[0], dst.dst.mac[1],
			  dst.dst.mac[2], dst.dst.mac[3], dst.dst.mac[4],
			  dst.dst.mac[5], dst.dst.vlan);
		free(dn);
	}
}

void b1b_fdb_free(struct b1b_bond_session *const bs)
{
	if (bs->fdbtree != NULL) {
		savl_free(&bs->fdbtree, b1b_fdb_free_cb);
		bs->fdbtree = NULL;
	}
}
