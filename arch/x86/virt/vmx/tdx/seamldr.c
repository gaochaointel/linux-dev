// SPDX-License-Identifier: GPL-2.0
/*
 * P-SEAMLDR support for TDX Module management features like runtime updates
 *
 * Copyright (C) 2025 Intel Corporation
 */
#define pr_fmt(fmt)	"seamldr: " fmt

#include <linux/mm.h>
#include <linux/spinlock.h>

#include <asm/seamldr.h>

#include "seamcall_internal.h"

/* P-SEAMLDR SEAMCALL leaf function */
#define P_SEAMLDR_INFO			0x8000000000000000

/*
 * Serialize P-SEAMLDR calls since the hardware only allows a single CPU to
 * interact with P-SEAMLDR simultaneously.
 */
static DEFINE_RAW_SPINLOCK(seamldr_lock);

static int seamldr_call(u64 fn, struct tdx_module_args *args)
{
	/*
	 * Serialize P-SEAMLDR calls and disable interrupts as the calls
	 * can be made from IRQ context.
	 */
	guard(raw_spinlock_irqsave)(&seamldr_lock);
	return seamcall_prerr(fn, args);
}

int seamldr_get_info(struct seamldr_info *seamldr_info)
{
	struct tdx_module_args args = { .rcx = slow_virt_to_phys(seamldr_info) };

	return seamldr_call(P_SEAMLDR_INFO, &args);
}
EXPORT_SYMBOL_FOR_MODULES(seamldr_get_info, "tdx-host");

/**
 * seamldr_install_module - Install a new TDX module
 * @data: Pointer to the TDX module update blob. It should be vmalloc'd
 *        memory.
 * @size: Size of the TDX module update blob
 *
 * Returns 0 on success, negative error code on failure.
 */
int seamldr_install_module(const u8 *data, u32 size)
{
	if (WARN_ON_ONCE(!is_vmalloc_addr(data)))
		return -EINVAL;

	/* TODO: Update TDX Module here */
	return 0;
}
EXPORT_SYMBOL_FOR_MODULES(seamldr_install_module, "tdx-host");
