// SPDX-License-Identifier: GPL-2.0
/*
 * f2fs extent cache support
 *
 * Copyright (c) 2015 Motorola Mobility
 * Copyright (c) 2015 Samsung Electronics
 * Authors: Jaegeuk Kim <jaegeuk@kernel.org>
 *          Chao Yu <chao2.yu@samsung.com>
 */

#include <linux/fs.h>
#include <linux/f2fs_fs.h>

#include "f2fs.h"
#include "node.h"
#include <trace/events/f2fs.h>

static struct rb_entry *__lookup_rb_tree_fast(struct rb_entry *cached_re,
							unsigned int ofs)
{
	if (cached_re) {
		if (cached_re->ofs <= ofs &&
				cached_re->ofs + cached_re->len > ofs) {
			return cached_re;
		}
	}
	return NULL;
}

static struct rb_entry *__lookup_rb_tree_slow(struct rb_root_cached *root,
							unsigned int ofs)
{
	struct rb_node *node = root->rb_root.rb_node;
	struct rb_entry *re;

	while (node) {
		re = rb_entry(node, struct rb_entry, rb_node);

		if (ofs < re->ofs)
			node = node->rb_left;
		else if (ofs >= re->ofs + re->len)
			node = node->rb_right;
		else
			return re;
	}
	return NULL;
}

struct rb_entry *f2fs_lookup_rb_tree(struct rb_root_cached *root,
				struct rb_entry *cached_re, unsigned int ofs)
{
	struct rb_entry *re;

	re = __lookup_rb_tree_fast(cached_re, ofs);
	if (!re)
		return __lookup_rb_tree_slow(root, ofs);

	return re;
}

struct rb_node **f2fs_lookup_rb_tree_for_insert(struct f2fs_sb_info *sbi,
				struct rb_root_cached *root,
				struct rb_node **parent,
				unsigned int ofs, bool *leftmost)
{
	struct rb_node **p = &root->rb_root.rb_node;
	struct rb_entry *re;

	while (*p) {
		*parent = *p;
		re = rb_entry(*parent, struct rb_entry, rb_node);

		if (ofs < re->ofs) {
			p = &(*p)->rb_left;
		} else if (ofs >= re->ofs + re->len) {
			p = &(*p)->rb_right;
			*leftmost = false;
		} else {
			f2fs_bug_on(sbi, 1);
		}
	}

	return p;
}

/*
 * lookup rb entry in position of @ofs in rb-tree,
 * if hit, return the entry, otherwise, return NULL
 * @prev_ex: extent before ofs
 * @next_ex: extent after ofs
 * @insert_p: insert point for new extent at ofs
 * in order to simpfy the insertion after.
 * tree must stay unchanged between lookup and insertion.
 */
struct rb_entry *f2fs_lookup_rb_tree_ret(struct rb_root_cached *root,
				struct rb_entry *cached_re,
				unsigned int ofs,
				struct rb_entry **prev_entry,
				struct rb_entry **next_entry,
				struct rb_node ***insert_p,
				struct rb_node **insert_parent,
				bool force, bool *leftmost)
{
	struct rb_node **pnode = &root->rb_root.rb_node;
	struct rb_node *parent = NULL, *tmp_node;
	struct rb_entry *re = cached_re;

	*insert_p = NULL;
	*insert_parent = NULL;
	*prev_entry = NULL;
	*next_entry = NULL;

	if (RB_EMPTY_ROOT(&root->rb_root))
		return NULL;

	if (re) {
		if (re->ofs <= ofs && re->ofs + re->len > ofs)
			goto lookup_neighbors;
	}

	if (leftmost)
		*leftmost = true;

	while (*pnode) {
		parent = *pnode;
		re = rb_entry(*pnode, struct rb_entry, rb_node);

		if (ofs < re->ofs) {
			pnode = &(*pnode)->rb_left;
		} else if (ofs >= re->ofs + re->len) {
			pnode = &(*pnode)->rb_right;
			if (leftmost)
				*leftmost = false;
		} else {
			goto lookup_neighbors;
		}
	}

	*insert_p = pnode;
	*insert_parent = parent;

	re = rb_entry(parent, struct rb_entry, rb_node);
	tmp_node = parent;
	if (parent && ofs > re->ofs)
		tmp_node = rb_next(parent);
	*next_entry = rb_entry_safe(tmp_node, struct rb_entry, rb_node);

	tmp_node = parent;
	if (parent && ofs < re->ofs)
		tmp_node = rb_prev(parent);
	*prev_entry = rb_entry_safe(tmp_node, struct rb_entry, rb_node);
	return NULL;

lookup_neighbors:
	if (ofs == re->ofs || force) {
		/* lookup prev node for merging backward later */
		tmp_node = rb_prev(&re->rb_node);
		*prev_entry = rb_entry_safe(tmp_node, struct rb_entry, rb_node);
	}
	if (ofs == re->ofs + re->len - 1 || force) {
		/* lookup next node for merging frontward later */
		tmp_node = rb_next(&re->rb_node);
		*next_entry = rb_entry_safe(tmp_node, struct rb_entry, rb_node);
	}
	return re;
}

bool f2fs_check_rb_tree_consistence(struct f2fs_sb_info *sbi,
						struct rb_root_cached *root)
{
#ifdef CONFIG_F2FS_CHECK_FS
	struct rb_node *cur = rb_first_cached(root), *next;
	struct rb_entry *cur_re, *next_re;

	if (!cur)
		return true;

	while (cur) {
		next = rb_next(cur);
		if (!next)
			return true;

		cur_re = rb_entry(cur, struct rb_entry, rb_node);
		next_re = rb_entry(next, struct rb_entry, rb_node);

		if (cur_re->ofs + cur_re->len > next_re->ofs) {
			f2fs_info(sbi, "inconsistent rbtree, cur(%u, %u) next(%u, %u)",
				  cur_re->ofs, cur_re->len,
				  next_re->ofs, next_re->len);
			return false;
		}

		cur = next;
	}
#endif
	return true;
}

static struct kmem_cache *extent_tree_slab;
static struct kmem_cache *extent_node_slab;

#define SAME_AGE_REGION		1024
#define F2FS_EXTENT_AGE_INVALID	ULLONG_MAX
#define LAST_AGE_WEIGHT		30

static void f2fs_init_age_extent_tree(struct inode *inode);
static void f2fs_drop_age_extent_tree(struct inode *inode);
static unsigned int f2fs_destroy_age_extent_node(struct inode *inode);
static void f2fs_destroy_age_extent_tree(struct inode *inode);

static struct extent_node *__attach_extent_node(struct f2fs_sb_info *sbi,
				struct extent_tree *et, struct extent_info *ei,
				struct rb_node *parent, struct rb_node **p,
				bool leftmost)
{
	struct extent_node *en;

	en = kmem_cache_alloc(extent_node_slab, GFP_ATOMIC);
	if (!en)
		return NULL;

	en->ei = *ei;
	INIT_LIST_HEAD(&en->list);
	en->et = et;

	rb_link_node(&en->rb_node, parent, p);
	rb_insert_color_cached(&en->rb_node, &et->root, leftmost);
	atomic_inc(&et->node_cnt);
	atomic_inc(&sbi->total_ext_node);
	return en;
}

static void __detach_extent_node(struct f2fs_sb_info *sbi,
				struct extent_tree *et, struct extent_node *en)
{
	rb_erase_cached(&en->rb_node, &et->root);
	atomic_dec(&et->node_cnt);
	atomic_dec(&sbi->total_ext_node);

	if (et->cached_en == en)
		et->cached_en = NULL;
	kmem_cache_free(extent_node_slab, en);
}

/*
 * Flow to release an extent_node:
 * 1. list_del_init
 * 2. __detach_extent_node
 * 3. kmem_cache_free.
 */
static void __release_extent_node(struct f2fs_sb_info *sbi,
			struct extent_tree *et, struct extent_node *en)
{
	spin_lock(&sbi->extent_lock);
	f2fs_bug_on(sbi, list_empty(&en->list));
	list_del_init(&en->list);
	spin_unlock(&sbi->extent_lock);

	__detach_extent_node(sbi, et, en);
}

static struct extent_tree *__grab_extent_tree(struct inode *inode)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct extent_tree *et;
	nid_t ino = inode->i_ino;

	mutex_lock(&sbi->extent_tree_lock);
	et = radix_tree_lookup(&sbi->extent_tree_root, ino);
	if (!et) {
		et = f2fs_kmem_cache_alloc(extent_tree_slab, GFP_NOFS);
		f2fs_radix_tree_insert(&sbi->extent_tree_root, ino, et);
		memset(et, 0, sizeof(struct extent_tree));
		et->ino = ino;
		et->root = RB_ROOT_CACHED;
		et->cached_en = NULL;
		rwlock_init(&et->lock);
		INIT_LIST_HEAD(&et->list);
		atomic_set(&et->node_cnt, 0);
		atomic_inc(&sbi->total_ext_tree);
	} else {
		atomic_dec(&sbi->total_zombie_tree);
		list_del_init(&et->list);
	}
	mutex_unlock(&sbi->extent_tree_lock);

	/* never died until evict_inode */
	F2FS_I(inode)->extent_tree = et;

	return et;
}

static struct extent_node *__init_extent_tree(struct f2fs_sb_info *sbi,
				struct extent_tree *et, struct extent_info *ei)
{
	struct rb_node **p = &et->root.rb_root.rb_node;
	struct extent_node *en;

	en = __attach_extent_node(sbi, et, ei, NULL, p, true);
	if (!en)
		return NULL;

	et->largest = en->ei;
	et->cached_en = en;
	return en;
}

static unsigned int __free_extent_tree(struct f2fs_sb_info *sbi,
					struct extent_tree *et)
{
	struct rb_node *node, *next;
	struct extent_node *en;
	unsigned int count = atomic_read(&et->node_cnt);

	node = rb_first_cached(&et->root);
	while (node) {
		next = rb_next(node);
		en = rb_entry(node, struct extent_node, rb_node);
		__release_extent_node(sbi, et, en);
		node = next;
	}

	return count - atomic_read(&et->node_cnt);
}

static void __drop_largest_extent(struct extent_tree *et,
					pgoff_t fofs, unsigned int len)
{
	if (fofs < et->largest.fofs + et->largest.len &&
			fofs + len > et->largest.fofs) {
		et->largest.len = 0;
		et->largest_updated = true;
	}
}

/* return true, if inode page is changed */
static bool __f2fs_init_extent_tree(struct inode *inode, struct f2fs_extent *i_ext)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct extent_tree *et;
	struct extent_node *en;
	struct extent_info ei;

	if (!f2fs_may_extent_tree(inode)) {
		/* drop largest extent */
		if (i_ext && i_ext->len) {
			i_ext->len = 0;
			return true;
		}
		return false;
	}

	et = __grab_extent_tree(inode);

	if (!i_ext || !i_ext->len)
		return false;

	get_extent_info(&ei, i_ext);

	write_lock(&et->lock);
	if (atomic_read(&et->node_cnt))
		goto out;

	en = __init_extent_tree(sbi, et, &ei);
	if (en) {
		spin_lock(&sbi->extent_lock);
		list_add_tail(&en->list, &sbi->extent_list);
		spin_unlock(&sbi->extent_lock);
	}
out:
	write_unlock(&et->lock);
	return false;
}

bool f2fs_init_extent_tree(struct inode *inode, struct f2fs_extent *i_ext)
{
	bool ret =  __f2fs_init_extent_tree(inode, i_ext);

	if (!F2FS_I(inode)->extent_tree)
		set_inode_flag(inode, FI_NO_EXTENT);

	f2fs_init_age_extent_tree(inode);

	return ret;
}

static bool f2fs_lookup_extent_tree(struct inode *inode, pgoff_t pgofs,
							struct extent_info *ei)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct extent_tree *et = F2FS_I(inode)->extent_tree;
	struct extent_node *en;
	bool ret = false;

	if (!et)
		return false;

	trace_f2fs_lookup_extent_tree_start(inode, pgofs);

	read_lock(&et->lock);

	if (et->largest.fofs <= pgofs &&
			et->largest.fofs + et->largest.len > pgofs) {
		*ei = et->largest;
		ret = true;
		stat_inc_largest_node_hit(sbi);
		goto out;
	}

	en = (struct extent_node *)f2fs_lookup_rb_tree(&et->root,
				(struct rb_entry *)et->cached_en, pgofs);
	if (!en)
		goto out;

	if (en == et->cached_en)
		stat_inc_cached_node_hit(sbi);
	else
		stat_inc_rbtree_node_hit(sbi);

	*ei = en->ei;
	spin_lock(&sbi->extent_lock);
	if (!list_empty(&en->list)) {
		list_move_tail(&en->list, &sbi->extent_list);
		et->cached_en = en;
	}
	spin_unlock(&sbi->extent_lock);
	ret = true;
out:
	stat_inc_total_hit(sbi);
	read_unlock(&et->lock);

	trace_f2fs_lookup_extent_tree_end(inode, pgofs, ei);
	return ret;
}

static struct extent_node *__try_merge_extent_node(struct f2fs_sb_info *sbi,
				struct extent_tree *et, struct extent_info *ei,
				struct extent_node *prev_ex,
				struct extent_node *next_ex)
{
	struct extent_node *en = NULL;

	if (prev_ex && __is_back_mergeable(ei, &prev_ex->ei)) {
		prev_ex->ei.len += ei->len;
		ei = &prev_ex->ei;
		en = prev_ex;
	}

	if (next_ex && __is_front_mergeable(ei, &next_ex->ei)) {
		next_ex->ei.fofs = ei->fofs;
		next_ex->ei.blk = ei->blk;
		next_ex->ei.len += ei->len;
		if (en)
			__release_extent_node(sbi, et, prev_ex);

		en = next_ex;
	}

	if (!en)
		return NULL;

	__try_update_largest_extent(et, en);

	spin_lock(&sbi->extent_lock);
	if (!list_empty(&en->list)) {
		list_move_tail(&en->list, &sbi->extent_list);
		et->cached_en = en;
	}
	spin_unlock(&sbi->extent_lock);
	return en;
}

static struct extent_node *__insert_extent_tree(struct f2fs_sb_info *sbi,
				struct extent_tree *et, struct extent_info *ei,
				struct rb_node **insert_p,
				struct rb_node *insert_parent,
				bool leftmost)
{
	struct rb_node **p;
	struct rb_node *parent = NULL;
	struct extent_node *en = NULL;

	if (insert_p && insert_parent) {
		parent = insert_parent;
		p = insert_p;
		goto do_insert;
	}

	leftmost = true;

	p = f2fs_lookup_rb_tree_for_insert(sbi, &et->root, &parent,
						ei->fofs, &leftmost);
do_insert:
	en = __attach_extent_node(sbi, et, ei, parent, p, leftmost);
	if (!en)
		return NULL;

	__try_update_largest_extent(et, en);

	/* update in global extent list */
	spin_lock(&sbi->extent_lock);
	list_add_tail(&en->list, &sbi->extent_list);
	et->cached_en = en;
	spin_unlock(&sbi->extent_lock);
	return en;
}

static void f2fs_update_extent_tree_range(struct inode *inode,
				pgoff_t fofs, block_t blkaddr, unsigned int len)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct extent_tree *et = F2FS_I(inode)->extent_tree;
	struct extent_node *en = NULL, *en1 = NULL;
	struct extent_node *prev_en = NULL, *next_en = NULL;
	struct extent_info ei, dei, prev;
	struct rb_node **insert_p = NULL, *insert_parent = NULL;
	unsigned int end = fofs + len;
	unsigned int pos = (unsigned int)fofs;
	bool updated = false;
	bool leftmost = false;

	if (!et)
		return;

	trace_f2fs_update_extent_tree_range(inode, fofs, blkaddr, len);

	write_lock(&et->lock);

	if (is_inode_flag_set(inode, FI_NO_EXTENT)) {
		write_unlock(&et->lock);
		return;
	}

	prev = et->largest;
	dei.len = 0;

	/*
	 * drop largest extent before lookup, in case it's already
	 * been shrunk from extent tree
	 */
	__drop_largest_extent(et, fofs, len);

	/* 1. lookup first extent node in range [fofs, fofs + len - 1] */
	en = (struct extent_node *)f2fs_lookup_rb_tree_ret(&et->root,
					(struct rb_entry *)et->cached_en, fofs,
					(struct rb_entry **)&prev_en,
					(struct rb_entry **)&next_en,
					&insert_p, &insert_parent, false,
					&leftmost);
	if (!en)
		en = next_en;

	/* 2. invlidate all extent nodes in range [fofs, fofs + len - 1] */
	while (en && en->ei.fofs < end) {
		unsigned int org_end;
		int parts = 0;	/* # of parts current extent split into */

		next_en = en1 = NULL;

		dei = en->ei;
		org_end = dei.fofs + dei.len;
		f2fs_bug_on(sbi, pos >= org_end);

		if (pos > dei.fofs &&	pos - dei.fofs >= F2FS_MIN_EXTENT_LEN) {
			en->ei.len = pos - en->ei.fofs;
			prev_en = en;
			parts = 1;
		}

		if (end < org_end && org_end - end >= F2FS_MIN_EXTENT_LEN) {
			if (parts) {
				set_extent_info(&ei, end,
						end - dei.fofs + dei.blk,
						org_end - end);
				en1 = __insert_extent_tree(sbi, et, &ei,
							NULL, NULL, true);
				next_en = en1;
			} else {
				en->ei.fofs = end;
				en->ei.blk += end - dei.fofs;
				en->ei.len -= end - dei.fofs;
				next_en = en;
			}
			parts++;
		}

		if (!next_en) {
			struct rb_node *node = rb_next(&en->rb_node);

			next_en = rb_entry_safe(node, struct extent_node,
						rb_node);
		}

		if (parts)
			__try_update_largest_extent(et, en);
		else
			__release_extent_node(sbi, et, en);

		/*
		 * if original extent is split into zero or two parts, extent
		 * tree has been altered by deletion or insertion, therefore
		 * invalidate pointers regard to tree.
		 */
		if (parts != 1) {
			insert_p = NULL;
			insert_parent = NULL;
		}
		en = next_en;
	}

	/* 3. update extent in extent cache */
	if (blkaddr) {

		set_extent_info(&ei, fofs, blkaddr, len);
		if (!__try_merge_extent_node(sbi, et, &ei, prev_en, next_en))
			__insert_extent_tree(sbi, et, &ei,
					insert_p, insert_parent, leftmost);

		/* give up extent_cache, if split and small updates happen */
		if (dei.len >= 1 &&
				prev.len < F2FS_MIN_EXTENT_LEN &&
				et->largest.len < F2FS_MIN_EXTENT_LEN) {
			et->largest.len = 0;
			et->largest_updated = true;
			set_inode_flag(inode, FI_NO_EXTENT);
		}
	}

	if (is_inode_flag_set(inode, FI_NO_EXTENT))
		__free_extent_tree(sbi, et);

	if (et->largest_updated) {
		et->largest_updated = false;
		updated = true;
	}

	write_unlock(&et->lock);

	if (updated)
		f2fs_mark_inode_dirty_sync(inode, true);
}

unsigned int f2fs_shrink_extent_tree(struct f2fs_sb_info *sbi, int nr_shrink)
{
	struct extent_tree *et, *next;
	struct extent_node *en;
	unsigned int node_cnt = 0, tree_cnt = 0;
	int remained;

	if (!test_opt(sbi, EXTENT_CACHE))
		return 0;

	if (!atomic_read(&sbi->total_zombie_tree))
		goto free_node;

	if (!mutex_trylock(&sbi->extent_tree_lock))
		goto out;

	/* 1. remove unreferenced extent tree */
	list_for_each_entry_safe(et, next, &sbi->zombie_list, list) {
		if (atomic_read(&et->node_cnt)) {
			write_lock(&et->lock);
			node_cnt += __free_extent_tree(sbi, et);
			write_unlock(&et->lock);
		}
		f2fs_bug_on(sbi, atomic_read(&et->node_cnt));
		list_del_init(&et->list);
		radix_tree_delete(&sbi->extent_tree_root, et->ino);
		kmem_cache_free(extent_tree_slab, et);
		atomic_dec(&sbi->total_ext_tree);
		atomic_dec(&sbi->total_zombie_tree);
		tree_cnt++;

		if (node_cnt + tree_cnt >= nr_shrink)
			goto unlock_out;
		cond_resched();
	}
	mutex_unlock(&sbi->extent_tree_lock);

free_node:
	/* 2. remove LRU extent entries */
	if (!mutex_trylock(&sbi->extent_tree_lock))
		goto out;

	remained = nr_shrink - (node_cnt + tree_cnt);

	spin_lock(&sbi->extent_lock);
	for (; remained > 0; remained--) {
		if (list_empty(&sbi->extent_list))
			break;
		en = list_first_entry(&sbi->extent_list,
					struct extent_node, list);
		et = en->et;
		if (!write_trylock(&et->lock)) {
			/* refresh this extent node's position in extent list */
			list_move_tail(&en->list, &sbi->extent_list);
			continue;
		}

		list_del_init(&en->list);
		spin_unlock(&sbi->extent_lock);

		__detach_extent_node(sbi, et, en);

		write_unlock(&et->lock);
		node_cnt++;
		spin_lock(&sbi->extent_lock);
	}
	spin_unlock(&sbi->extent_lock);

unlock_out:
	mutex_unlock(&sbi->extent_tree_lock);
out:
	trace_f2fs_shrink_extent_tree(sbi, node_cnt, tree_cnt);

	return node_cnt + tree_cnt;
}

unsigned int f2fs_destroy_extent_node(struct inode *inode)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct extent_tree *et = F2FS_I(inode)->extent_tree;
	unsigned int node_cnt = 0;

	if (et && atomic_read(&et->node_cnt)) {
		write_lock(&et->lock);
		node_cnt = __free_extent_tree(sbi, et);
		write_unlock(&et->lock);
	}

	return node_cnt + f2fs_destroy_age_extent_node(inode);
}

void f2fs_drop_extent_tree(struct inode *inode)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct extent_tree *et = F2FS_I(inode)->extent_tree;
	bool updated = false;

	if (f2fs_may_extent_tree(inode) && et) {
		write_lock(&et->lock);
		set_inode_flag(inode, FI_NO_EXTENT);
		__free_extent_tree(sbi, et);
		if (et->largest.len) {
			et->largest.len = 0;
			updated = true;
		}
		write_unlock(&et->lock);
	}
	f2fs_drop_age_extent_tree(inode);
	if (updated)
		f2fs_mark_inode_dirty_sync(inode, true);
}

void f2fs_destroy_extent_tree(struct inode *inode)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct extent_tree *et = F2FS_I(inode)->extent_tree;
	unsigned int node_cnt = 0;

	if (et) {
		if (inode->i_nlink && !is_bad_inode(inode) &&
					atomic_read(&et->node_cnt)) {
			mutex_lock(&sbi->extent_tree_lock);
			list_add_tail(&et->list, &sbi->zombie_list);
			atomic_inc(&sbi->total_zombie_tree);
			mutex_unlock(&sbi->extent_tree_lock);
		} else {
			/* free all extent info belong to this extent tree */
			node_cnt = f2fs_destroy_extent_node(inode);

			/* delete extent tree entry in radix tree */
			mutex_lock(&sbi->extent_tree_lock);
			f2fs_bug_on(sbi, atomic_read(&et->node_cnt));
			radix_tree_delete(&sbi->extent_tree_root, inode->i_ino);
			kmem_cache_free(extent_tree_slab, et);
			atomic_dec(&sbi->total_ext_tree);
			mutex_unlock(&sbi->extent_tree_lock);

			F2FS_I(inode)->extent_tree = NULL;
		}
	}

	/* age extent tree is independent from the read extent tree */
	f2fs_destroy_age_extent_tree(inode);

	trace_f2fs_destroy_extent_tree(inode, node_cnt);
}

bool f2fs_lookup_extent_cache(struct inode *inode, pgoff_t pgofs,
					struct extent_info *ei)
{
	if (!f2fs_may_extent_tree(inode))
		return false;

	return f2fs_lookup_extent_tree(inode, pgofs, ei);
}

void f2fs_update_extent_cache(struct dnode_of_data *dn)
{
	pgoff_t fofs;
	block_t blkaddr;

	if (!f2fs_may_extent_tree(dn->inode))
		return;

	if (dn->data_blkaddr == NEW_ADDR)
		blkaddr = NULL_ADDR;
	else
		blkaddr = dn->data_blkaddr;

	fofs = f2fs_start_bidx_of_node(ofs_of_node(dn->node_page), dn->inode) +
								dn->ofs_in_node;
	f2fs_update_extent_tree_range(dn->inode, fofs, blkaddr, 1);
}

void f2fs_update_extent_cache_range(struct dnode_of_data *dn,
				pgoff_t fofs, block_t blkaddr, unsigned int len)

{
	if (!f2fs_may_extent_tree(dn->inode))
		return;

	f2fs_update_extent_tree_range(dn->inode, fofs, blkaddr, len);
}

void f2fs_init_extent_cache_info(struct f2fs_sb_info *sbi)
{
	INIT_RADIX_TREE(&sbi->extent_tree_root, GFP_NOIO);
	mutex_init(&sbi->extent_tree_lock);
	INIT_LIST_HEAD(&sbi->extent_list);
	spin_lock_init(&sbi->extent_lock);
	atomic_set(&sbi->total_ext_tree, 0);
	INIT_LIST_HEAD(&sbi->zombie_list);
	atomic_set(&sbi->total_zombie_tree, 0);
	atomic_set(&sbi->total_ext_node, 0);

	INIT_RADIX_TREE(&sbi->age_extent_tree.extent_tree_root, GFP_NOIO);
	mutex_init(&sbi->age_extent_tree.extent_tree_lock);
	INIT_LIST_HEAD(&sbi->age_extent_tree.extent_list);
	spin_lock_init(&sbi->age_extent_tree.extent_lock);
	atomic_set(&sbi->age_extent_tree.total_ext_tree, 0);
	INIT_LIST_HEAD(&sbi->age_extent_tree.zombie_list);
	atomic_set(&sbi->age_extent_tree.total_zombie_tree, 0);
	atomic_set(&sbi->age_extent_tree.total_ext_node, 0);
	atomic64_set(&sbi->allocated_data_blocks, 0);
	sbi->hot_data_age_threshold = 262144;
	sbi->warm_data_age_threshold = 2621440;
	sbi->last_age_weight = LAST_AGE_WEIGHT;
}


/*
 * Block age extent cache
 *
 * This cache is kept separately from the read extent cache above.  It is
 * used by the age-threshold allocation paths and is not persisted in the
 * inode.
 */
static bool f2fs_may_age_extent_tree(struct inode *inode)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);

	if (!test_opt(sbi, AGE_EXTENT_CACHE) ||
			f2fs_compressed_file(inode))
		return false;

	if (list_empty(&sbi->s_list))
		return false;

	if (file_is_cold(inode))
		return false;

	return S_ISREG(inode->i_mode) || S_ISDIR(inode->i_mode);
}

static struct extent_node *__age_attach_extent_node(struct f2fs_sb_info *sbi,
				struct extent_tree *et, struct extent_info *ei,
				struct rb_node *parent, struct rb_node **p,
				bool leftmost)
{
	struct extent_node *en;

	en = kmem_cache_alloc(extent_node_slab, GFP_ATOMIC);
	if (!en)
		return NULL;

	en->ei = *ei;
	INIT_LIST_HEAD(&en->list);
	en->et = et;

	rb_link_node(&en->rb_node, parent, p);
	rb_insert_color_cached(&en->rb_node, &et->root, leftmost);
	atomic_inc(&et->node_cnt);
	atomic_inc(&sbi->age_extent_tree.total_ext_node);
	return en;
}

static void __age_detach_extent_node(struct f2fs_sb_info *sbi,
				struct extent_tree *et, struct extent_node *en)
{
	f2fs_bug_on(sbi, !et);

	rb_erase_cached(&en->rb_node, &et->root);
	if (et->cached_en == en)
		et->cached_en = NULL;
	atomic_dec(&et->node_cnt);
	atomic_dec(&sbi->age_extent_tree.total_ext_node);
	kmem_cache_free(extent_node_slab, en);
}

static void __age_release_extent_node(struct f2fs_sb_info *sbi,
				struct extent_tree *et, struct extent_node *en)
{
	if (!list_empty(&en->list)) {
		spin_lock(&sbi->age_extent_tree.extent_lock);
		list_del_init(&en->list);
		spin_unlock(&sbi->age_extent_tree.extent_lock);
	}
	__age_detach_extent_node(sbi, et, en);
}

static struct extent_tree *__age_grab_extent_tree(struct inode *inode)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct extent_tree_info *eti = &sbi->age_extent_tree;
	struct extent_tree *et;

	mutex_lock(&eti->extent_tree_lock);
	et = radix_tree_lookup(&eti->extent_tree_root, inode->i_ino);
	if (!et) {
		et = kmem_cache_alloc(extent_tree_slab, GFP_NOFS);
		if (!et) {
			mutex_unlock(&eti->extent_tree_lock);
			return NULL;
		}
		et->ino = inode->i_ino;
		et->root = RB_ROOT_CACHED;
		et->cached_en = NULL;
		memset(&et->largest, 0, sizeof(et->largest));
		INIT_LIST_HEAD(&et->list);
		rwlock_init(&et->lock);
		atomic_set(&et->node_cnt, 0);
		et->largest_updated = false;
		f2fs_radix_tree_insert(&eti->extent_tree_root, inode->i_ino, et);
		atomic_inc(&eti->total_ext_tree);
	} else {
		f2fs_bug_on(sbi, !atomic_read(&eti->total_zombie_tree));
		atomic_dec(&eti->total_zombie_tree);
		list_del_init(&et->list);
	}
	mutex_unlock(&eti->extent_tree_lock);

	F2FS_I(inode)->age_extent_tree = et;
	return et;
}

static struct extent_node *__age_lookup_extent_node(struct extent_tree *et,
						struct extent_node *cached_en,
						pgoff_t pgofs)
{
	struct rb_node *node = et->root.rb_root.rb_node;
	struct extent_node *en = cached_en;

	if (en && en->ei.fofs <= pgofs && en->ei.fofs + en->ei.len > pgofs)
		return en;

	while (node) {
		en = rb_entry(node, struct extent_node, rb_node);

		if (pgofs < en->ei.fofs)
			node = node->rb_left;
		else if (pgofs >= en->ei.fofs + en->ei.len)
			node = node->rb_right;
		else
			return en;
	}
	return NULL;
}

static struct extent_node *__age_lookup_extent_node_ret(
				struct extent_tree *et,
				struct extent_node *cached_en,
				pgoff_t pgofs,
				struct extent_node **prev_entry,
				struct extent_node **next_entry,
				struct rb_node ***insert_p,
				struct rb_node **insert_parent,
				bool force)
{
	struct rb_node **pnode = &et->root.rb_root.rb_node;
	struct rb_node *parent = NULL, *tmp_node;
	struct extent_node *en = cached_en;

	*insert_p = NULL;
	*insert_parent = NULL;
	*prev_entry = NULL;
	*next_entry = NULL;

	if (RB_EMPTY_ROOT(&et->root.rb_root))
		return NULL;

	if (en && en->ei.fofs <= pgofs &&
			en->ei.fofs + en->ei.len > pgofs)
		goto lookup_neighbors;

	while (*pnode) {
		parent = *pnode;
		en = rb_entry(*pnode, struct extent_node, rb_node);

		if (pgofs < en->ei.fofs)
			pnode = &(*pnode)->rb_left;
		else if (pgofs >= en->ei.fofs + en->ei.len)
			pnode = &(*pnode)->rb_right;
		else
			goto lookup_neighbors;
	}

	*insert_p = pnode;
	*insert_parent = parent;

	en = rb_entry(parent, struct extent_node, rb_node);
	tmp_node = parent;
	if (parent && pgofs > en->ei.fofs)
		tmp_node = rb_next(parent);
	*next_entry = rb_entry_safe(tmp_node, struct extent_node, rb_node);

	tmp_node = parent;
	if (parent && pgofs < en->ei.fofs)
		tmp_node = rb_prev(parent);
	*prev_entry = rb_entry_safe(tmp_node, struct extent_node, rb_node);
	return NULL;

lookup_neighbors:
	if (pgofs == en->ei.fofs || force) {
		tmp_node = rb_prev(&en->rb_node);
		*prev_entry = rb_entry_safe(tmp_node, struct extent_node, rb_node);
	}
	if (pgofs == en->ei.fofs + en->ei.len - 1 || force) {
		tmp_node = rb_next(&en->rb_node);
		*next_entry = rb_entry_safe(tmp_node, struct extent_node, rb_node);
	}
	return en;
}

static bool __age_lookup_extent_tree(struct inode *inode, pgoff_t pgofs,
						struct extent_info *ei)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct extent_tree *et = F2FS_I(inode)->age_extent_tree;
	struct extent_node *en;

	if (!et)
		return false;

	read_lock(&et->lock);
	en = __age_lookup_extent_node(et, et->cached_en, pgofs);
	if (!en) {
		read_unlock(&et->lock);
		return false;
	}

	*ei = en->ei;
	spin_lock(&sbi->age_extent_tree.extent_lock);
	if (!list_empty(&en->list)) {
		list_move_tail(&en->list, &sbi->age_extent_tree.extent_list);
		et->cached_en = en;
	}
	spin_unlock(&sbi->age_extent_tree.extent_lock);
	read_unlock(&et->lock);
	return true;
}

static unsigned long long __age_diff(unsigned long long a,
						unsigned long long b)
{
	return a > b ? a - b : b - a;
}

static bool __age_is_mergeable(struct extent_info *back,
						struct extent_info *front)
{
	return back->fofs + back->len == front->fofs &&
		__age_diff(back->age, front->age) <= SAME_AGE_REGION &&
		__age_diff(back->last_blocks, front->last_blocks) <=
							SAME_AGE_REGION;
}

static struct extent_node *__age_try_merge_extent_node(
				struct f2fs_sb_info *sbi,
				struct extent_tree *et, struct extent_info *ei,
				struct extent_node *prev_en,
				struct extent_node *next_en)
{
	struct extent_node *en = NULL;

	if (prev_en && __age_is_mergeable(&prev_en->ei, ei)) {
		prev_en->ei.len += ei->len;
		ei = &prev_en->ei;
		en = prev_en;
	}

	if (next_en && __age_is_mergeable(ei, &next_en->ei)) {
		next_en->ei.fofs = ei->fofs;
		next_en->ei.len += ei->len;
		if (en)
			__age_release_extent_node(sbi, et, prev_en);
		en = next_en;
	}

	if (!en)
		return NULL;

	spin_lock(&sbi->age_extent_tree.extent_lock);
	if (!list_empty(&en->list)) {
		list_move_tail(&en->list, &sbi->age_extent_tree.extent_list);
		et->cached_en = en;
	}
	spin_unlock(&sbi->age_extent_tree.extent_lock);
	return en;
}

static struct extent_node *__age_insert_extent_tree(
				struct f2fs_sb_info *sbi,
				struct extent_tree *et, struct extent_info *ei,
				struct rb_node **insert_p,
				struct rb_node *insert_parent,
				bool leftmost)
{
	struct rb_node **p;
	struct rb_node *parent = NULL;
	struct extent_node *en;

	if (insert_p && insert_parent) {
		parent = insert_parent;
		p = insert_p;
		goto do_insert;
	}

	leftmost = true;
	p = &et->root.rb_root.rb_node;
	while (*p) {
		parent = *p;
		en = rb_entry(parent, struct extent_node, rb_node);
		if (ei->fofs < en->ei.fofs) {
			p = &(*p)->rb_left;
		} else {
			p = &(*p)->rb_right;
			leftmost = false;
		}
	}

do_insert:
	en = __age_attach_extent_node(sbi, et, ei, parent, p, leftmost);
	if (!en)
		return NULL;

	spin_lock(&sbi->age_extent_tree.extent_lock);
	list_add_tail(&en->list, &sbi->age_extent_tree.extent_list);
	et->cached_en = en;
	spin_unlock(&sbi->age_extent_tree.extent_lock);
	return en;
}

static void __age_update_extent_tree_range(struct inode *inode,
				pgoff_t fofs, unsigned int len,
				struct extent_info *ei)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct extent_tree *et = F2FS_I(inode)->age_extent_tree;
	struct extent_node *en = NULL, *en1 = NULL;
	struct extent_node *prev_en = NULL, *next_en = NULL;
	struct extent_info dei, split;
	struct rb_node **insert_p = NULL, *insert_parent = NULL;
	unsigned int end = fofs + len;
	unsigned int pos = (unsigned int)fofs;

	if (!et)
		return;

	write_lock(&et->lock);

	en = __age_lookup_extent_node_ret(et, et->cached_en, fofs,
					&prev_en, &next_en,
					&insert_p, &insert_parent, false);
	if (!en)
		en = next_en;

	while (en && en->ei.fofs < end) {
		unsigned int org_end;
		int parts = 0;

		next_en = en1 = NULL;
		dei = en->ei;
		org_end = dei.fofs + dei.len;
		f2fs_bug_on(sbi, pos >= org_end);

		if (pos > dei.fofs) {
			en->ei.len = pos - en->ei.fofs;
			prev_en = en;
			parts = 1;
		}

		if (end < org_end) {
			if (parts) {
				split = dei;
				split.fofs = end;
				split.len = org_end - end;
				en1 = __age_insert_extent_tree(sbi, et, &split,
							NULL, NULL, true);
				next_en = en1;
			} else {
				en->ei.fofs = end;
				en->ei.len -= end - dei.fofs;
				next_en = en;
			}
			parts++;
		}

		if (!next_en) {
			struct rb_node *node = rb_next(&en->rb_node);

			next_en = rb_entry_safe(node, struct extent_node,
						rb_node);
		}

		if (!parts)
			__age_release_extent_node(sbi, et, en);

		if (parts != 1) {
			insert_p = NULL;
			insert_parent = NULL;
		}
		en = next_en;
	}

	if (ei->last_blocks == F2FS_EXTENT_AGE_INVALID)
		goto out;

	if (!__age_try_merge_extent_node(sbi, et, ei, prev_en, next_en))
		__age_insert_extent_tree(sbi, et, ei, NULL, NULL, false);
out:
	write_unlock(&et->lock);
}

static unsigned long long __age_calculate_block_age(struct f2fs_sb_info *sbi,
				unsigned long long new,
				unsigned long long old)
{
	unsigned int rem_old, rem_new;
	unsigned long long res;
	unsigned int weight = sbi->last_age_weight;

	res = div_u64_rem(new, 100, &rem_new) * (100 - weight)
		+ div_u64_rem(old, 100, &rem_old) * weight;

	if (rem_new)
		res += rem_new * (100 - weight) / 100;
	if (rem_old)
		res += rem_old * weight / 100;

	return res;
}

/* This returns a new age and allocated blocks in ei */
static int __age_get_new_block_age(struct inode *inode,
				struct extent_info *ei, block_t blkaddr)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	loff_t f_size = i_size_read(inode);
	unsigned long long cur_blocks =
				atomic64_read(&sbi->allocated_data_blocks);
	struct extent_info tei = *ei;	/* only fofs and len are valid */

	/*
	 * When I/O is not aligned to a PAGE_SIZE, update will happen to the last
	 * file block even in seq write. So don't record age for newly last file
	 * block here.
	 */
	if ((f_size >> PAGE_SHIFT) == ei->fofs && f_size & (PAGE_SIZE - 1) &&
			blkaddr == NEW_ADDR)
		return -EINVAL;

	if (__age_lookup_extent_tree(inode, ei->fofs, &tei)) {
		unsigned long long cur_age;

		if (cur_blocks >= tei.last_blocks)
			cur_age = cur_blocks - tei.last_blocks;
		else
			/* allocated_data_blocks overflow */
			cur_age = (ULLONG_MAX - 1) - tei.last_blocks + cur_blocks;

		if (tei.age)
			ei->age = __age_calculate_block_age(sbi, cur_age, tei.age);
		else
			ei->age = cur_age;
		ei->last_blocks = cur_blocks;
		WARN_ON(ei->age > cur_blocks);
		return 0;
	}

	f2fs_bug_on(sbi, blkaddr == NULL_ADDR);

	/* the data block was allocated for the first time */
	if (blkaddr == NEW_ADDR)
		goto out;

	if (__is_valid_data_blkaddr(blkaddr) &&
	    !f2fs_is_valid_blkaddr(sbi, blkaddr, DATA_GENERIC_ENHANCE))
		return -EINVAL;
out:
	/*
	 * init block age with zero, this can happen when the block age extent
	 * was reclaimed due to memory constraint or system reboot
	 */
	ei->age = 0;
	ei->last_blocks = cur_blocks;
	return 0;
}

static unsigned int __age_free_extent_tree(struct f2fs_sb_info *sbi,
				struct extent_tree *et, unsigned int nr_shrink)
{
	struct rb_node *node, *next;
	struct extent_node *en;
	unsigned int count = 0;

	node = rb_first_cached(&et->root);
	while (node) {
		next = rb_next(node);
		en = rb_entry(node, struct extent_node, rb_node);
		__age_release_extent_node(sbi, et, en);
		count++;
		if (nr_shrink && count >= nr_shrink)
			break;
		node = next;
	}
	return count;
}

static void f2fs_init_age_extent_tree(struct inode *inode)
{
	if (!f2fs_may_age_extent_tree(inode))
		return;
	__age_grab_extent_tree(inode);
}

static void f2fs_drop_age_extent_tree(struct inode *inode)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct extent_tree *et = F2FS_I(inode)->age_extent_tree;

	if (!et)
		return;

	write_lock(&et->lock);
	__age_free_extent_tree(sbi, et, 0);
	write_unlock(&et->lock);
}

static unsigned int f2fs_destroy_age_extent_node(struct inode *inode)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct extent_tree *et = F2FS_I(inode)->age_extent_tree;
	unsigned int node_cnt = 0;

	if (!et || !atomic_read(&et->node_cnt))
		return 0;

	write_lock(&et->lock);
	node_cnt = __age_free_extent_tree(sbi, et, 0);
	write_unlock(&et->lock);
	return node_cnt;
}

static void f2fs_destroy_age_extent_tree(struct inode *inode)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct extent_tree_info *eti = &sbi->age_extent_tree;
	struct extent_tree *et = F2FS_I(inode)->age_extent_tree;

	if (!et)
		return;

	if (inode->i_nlink && !is_bad_inode(inode) &&
					atomic_read(&et->node_cnt)) {
		mutex_lock(&eti->extent_tree_lock);
		list_add_tail(&et->list, &eti->zombie_list);
		atomic_inc(&eti->total_zombie_tree);
		mutex_unlock(&eti->extent_tree_lock);
		return;
	}

	f2fs_destroy_age_extent_node(inode);

	mutex_lock(&eti->extent_tree_lock);
	f2fs_bug_on(sbi, atomic_read(&et->node_cnt));
	radix_tree_delete(&eti->extent_tree_root, inode->i_ino);
	kmem_cache_free(extent_tree_slab, et);
	atomic_dec(&eti->total_ext_tree);
	mutex_unlock(&eti->extent_tree_lock);

	F2FS_I(inode)->age_extent_tree = NULL;
}

bool f2fs_lookup_age_extent_cache(struct inode *inode, pgoff_t pgofs,
						struct extent_info *ei)
{
	if (!f2fs_may_age_extent_tree(inode))
		return false;
	return __age_lookup_extent_tree(inode, pgofs, ei);
}

void f2fs_update_age_extent_cache(struct dnode_of_data *dn)
{
	struct extent_info ei;
	pgoff_t fofs;

	if (!f2fs_may_age_extent_tree(dn->inode))
		return;

	fofs = f2fs_start_bidx_of_node(ofs_of_node(dn->node_page), dn->inode) +
								dn->ofs_in_node;
	ei.fofs = fofs;
	ei.len = 1;
	if (__age_get_new_block_age(dn->inode, &ei, dn->data_blkaddr))
		return;
	__age_update_extent_tree_range(dn->inode, fofs, 1, &ei);
}

void f2fs_update_age_extent_cache_range(struct dnode_of_data *dn,
				pgoff_t fofs, unsigned int len)
{
	struct extent_info ei;

	if (!len || !f2fs_may_age_extent_tree(dn->inode))
		return;

	ei.fofs = fofs;
	ei.len = len;
	ei.age = 0;
	ei.last_blocks = F2FS_EXTENT_AGE_INVALID;
	__age_update_extent_tree_range(dn->inode, fofs, len, &ei);
}

unsigned int f2fs_shrink_age_extent_tree(struct f2fs_sb_info *sbi,
						int nr_shrink)
{
	struct extent_tree_info *eti = &sbi->age_extent_tree;
	struct extent_tree *et, *next;
	struct extent_node *en;
	unsigned int node_cnt = 0, tree_cnt = 0;
	int remained;

	if (!test_opt(sbi, AGE_EXTENT_CACHE))
		return 0;

	if (!atomic_read(&eti->total_zombie_tree))
		goto free_node;

	if (!mutex_trylock(&eti->extent_tree_lock))
		goto out;

	list_for_each_entry_safe(et, next, &eti->zombie_list, list) {
		if (atomic_read(&et->node_cnt)) {
			write_lock(&et->lock);
			node_cnt += __age_free_extent_tree(sbi, et, 0);
			write_unlock(&et->lock);
		}
		f2fs_bug_on(sbi, atomic_read(&et->node_cnt));
		list_del_init(&et->list);
		radix_tree_delete(&eti->extent_tree_root, et->ino);
		kmem_cache_free(extent_tree_slab, et);
		atomic_dec(&eti->total_ext_tree);
		atomic_dec(&eti->total_zombie_tree);
		tree_cnt++;

		if (node_cnt + tree_cnt >= nr_shrink)
			goto unlock_out;
		cond_resched();
	}
	mutex_unlock(&eti->extent_tree_lock);

free_node:
	if (!mutex_trylock(&eti->extent_tree_lock))
		goto out;

	remained = nr_shrink - (node_cnt + tree_cnt);

	spin_lock(&eti->extent_lock);
	for (; remained > 0; remained--) {
		if (list_empty(&eti->extent_list))
			break;
		en = list_first_entry(&eti->extent_list,
					struct extent_node, list);
		et = en->et;
		if (!write_trylock(&et->lock)) {
			list_move_tail(&en->list, &eti->extent_list);
			continue;
		}

		list_del_init(&en->list);
		spin_unlock(&eti->extent_lock);

		__age_detach_extent_node(sbi, et, en);

		write_unlock(&et->lock);
		node_cnt++;
		spin_lock(&eti->extent_lock);
	}
	spin_unlock(&eti->extent_lock);

unlock_out:
	mutex_unlock(&eti->extent_tree_lock);
out:
	return node_cnt + tree_cnt;
}

int __init f2fs_create_extent_cache(void)
{
	extent_tree_slab = f2fs_kmem_cache_create("f2fs_extent_tree",
			sizeof(struct extent_tree));
	if (!extent_tree_slab)
		return -ENOMEM;
	extent_node_slab = f2fs_kmem_cache_create("f2fs_extent_node",
			sizeof(struct extent_node));
	if (!extent_node_slab) {
		kmem_cache_destroy(extent_tree_slab);
		return -ENOMEM;
	}
	return 0;
}

void f2fs_destroy_extent_cache(void)
{
	kmem_cache_destroy(extent_node_slab);
	kmem_cache_destroy(extent_tree_slab);
}
