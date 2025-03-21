// SPDX-License-Identifier: GPL-2.0
/*
 * P-SEAMLDR support for TDX Module management features like runtime updates
 *
 * Copyright (C) 2025 Intel Corporation
 */
#define pr_fmt(fmt)	"seamldr: " fmt

#include <linux/spinlock.h>

#include "seamcall_internal.h"

/*
 * Serialize P-SEAMLDR calls since the hardware only allows a single CPU to
 * interact with P-SEAMLDR simultaneously.
 */
static DEFINE_RAW_SPINLOCK(seamldr_lock);

static __maybe_unused int seamldr_call(u64 fn, struct tdx_module_args *args)
{
	/*
	 * Serialize P-SEAMLDR calls and disable interrupts as the calls
	 * can be made from IRQ context.
	 */
	guard(raw_spinlock_irqsave)(&seamldr_lock);
	return seamcall_prerr(fn, args);
}
