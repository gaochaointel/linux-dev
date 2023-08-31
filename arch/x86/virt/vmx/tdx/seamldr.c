// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2025 Intel Corporation.
 *
 * Intel TDX module runtime update
 */
#define pr_fmt(fmt)	"seamldr: " fmt

#include <linux/irqflags.h>
#include <linux/mm.h>
#include <linux/types.h>

#include <asm/seamldr.h>

#include "seamcall.h"

/* P-SEAMLDR SEAMCALL leaf function */
#define P_SEAMLDR_INFO			0x8000000000000000

static struct seamldr_info seamldr_info __aligned(256);

static inline int seamldr_call(u64 fn, struct tdx_module_args *args)
{
	unsigned long flags;
	u64 vmcs;
	int ret;

	if (!is_seamldr_call(fn))
		return -EINVAL;

	/*
	 * SEAMRET from P-SEAMLDR invalidates the current VMCS.  Save/restore
	 * the VMCS across P-SEAMLDR SEAMCALLs to avoid clobbering KVM state.
	 * Disable interrupts as KVM is allowed to do VMREAD/VMWRITE in IRQ
	 * context (but not NMI context).
	 */
	local_irq_save(flags);

	asm goto("1: vmptrst %0\n\t"
		 _ASM_EXTABLE(1b, %l[error])
		 : "=m" (vmcs) : : "cc" : error);

	ret = seamldr_prerr(fn, args);

	/*
	 * Restore the current VMCS pointer.  VMPTSTR "returns" all ones if the
	 * current VMCS is invalid.
	 */
	if (vmcs != -1ULL) {
		asm goto("1: vmptrld %0\n\t"
			 "jna %l[error]\n\t"
			 _ASM_EXTABLE(1b, %l[error])
			 : : "m" (vmcs) : "cc" : error);
	}

	local_irq_restore(flags);
	return ret;

error:
	local_irq_restore(flags);

	WARN_ONCE(1, "Failed to save/restore the current VMCS");
	return -EIO;
}

const struct seamldr_info *seamldr_get_info(void)
{
	struct tdx_module_args args = { .rcx = __pa(&seamldr_info) };

	return seamldr_call(P_SEAMLDR_INFO, &args) ? NULL : &seamldr_info;
}
EXPORT_SYMBOL_FOR_MODULES(seamldr_get_info, "tdx-host");

/**
 * seamldr_install_module - Install a new TDX module
 * @data: Pointer to the TDX module binary data. It should be vmalloc'd
 *        memory.
 * @size: Size of the TDX module binary data
 *
 * Returns 0 on success, negative error code on failure.
 */
int seamldr_install_module(const u8 *data, u32 size)
{
	if (!is_vmalloc_addr(data))
		return -EINVAL;

	/* TODO: Update TDX Module here */
	return 0;
}
EXPORT_SYMBOL_FOR_MODULES(seamldr_install_module, "tdx-host");
