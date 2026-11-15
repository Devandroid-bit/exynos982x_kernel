/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SEC_MM_TUNE_H
#define _LINUX_SEC_MM_TUNE_H

#include <linux/types.h>
#include <linux/swap.h>

#ifdef CONFIG_SEC_MM_TUNE
void sec_mm_swapin_start(swp_entry_t entry);
void sec_mm_swapin_done(swp_entry_t entry);
void sec_mm_swapin_wait(swp_entry_t entry);
bool sec_mm_shrink_slab_bypass(int priority);
#else
static inline void sec_mm_swapin_start(swp_entry_t entry)
{
}

static inline void sec_mm_swapin_done(swp_entry_t entry)
{
}

static inline void sec_mm_swapin_wait(swp_entry_t entry)
{
}

static inline bool sec_mm_shrink_slab_bypass(int priority)
{
	return false;
}
#endif

#endif /* _LINUX_SEC_MM_TUNE_H */
