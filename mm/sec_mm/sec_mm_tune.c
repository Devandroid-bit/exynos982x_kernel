// SPDX-License-Identifier: GPL-2.0

#include <linux/atomic.h>
#include <linux/bitops.h>
#include <linux/hash.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/sec_mm_tune.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/wait.h>

#include "sec_mm.h"

#define NR_SWAP_HASH_BITS	8
#define NR_SWAP_WAITQUEUE	BIT(NR_SWAP_HASH_BITS)

static wait_queue_head_t swap_waitqueue[NR_SWAP_WAITQUEUE];
static atomic_t swapin_inflight[NR_SWAP_WAITQUEUE];
static bool sec_mm_tune_ready;

static int sec_mm_swap_hash_idx(swp_entry_t entry)
{
	return hash_long(swp_offset(entry), NR_SWAP_HASH_BITS);
}

void sec_mm_swapin_start(swp_entry_t entry)
{
	if (!sec_mm_tune_ready)
		return;
	atomic_inc(&swapin_inflight[sec_mm_swap_hash_idx(entry)]);
}

void sec_mm_swapin_done(swp_entry_t entry)
{
	int idx;

	if (!sec_mm_tune_ready)
		return;

	idx = sec_mm_swap_hash_idx(entry);
	atomic_dec(&swapin_inflight[idx]);
	wake_up(&swap_waitqueue[idx]);
}

void sec_mm_swapin_wait(swp_entry_t entry)
{
	DEFINE_WAIT(wait);
	int idx;

	if (!sec_mm_tune_ready) {
		cond_resched();
		return;
	}

	idx = sec_mm_swap_hash_idx(entry);
	if (!atomic_read(&swapin_inflight[idx])) {
		cond_resched();
		return;
	}

	prepare_to_wait(&swap_waitqueue[idx], &wait, TASK_UNINTERRUPTIBLE);
	if (atomic_read(&swapin_inflight[idx]))
		schedule();
	finish_wait(&swap_waitqueue[idx], &wait);
}

bool sec_mm_shrink_slab_bypass(int priority)
{
	if (!sec_mm_tune_ready)
		return false;

	return !current_is_kswapd() && priority > 0;
}

void init_sec_mm_tune(void)
{
	int i;

	for (i = 0; i < NR_SWAP_WAITQUEUE; i++)
		init_waitqueue_head(&swap_waitqueue[i]);

	sec_mm_tune_ready = true;
}

void exit_sec_mm_tune(void)
{
	int i;

	sec_mm_tune_ready = false;

	for (i = 0; i < NR_SWAP_WAITQUEUE; i++)
		wake_up_all(&swap_waitqueue[i]);
}
