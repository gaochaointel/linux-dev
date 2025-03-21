// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2025 Intel Corporation.
 *
 * Intel TDX module runtime update
 */
#define pr_fmt(fmt)	"seamldr: " fmt

#include <linux/irqflags.h>
#include <linux/types.h>

#include "seamcall.h"

static __maybe_unused int seamldr_call(u64 fn, struct tdx_module_args *args)
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
