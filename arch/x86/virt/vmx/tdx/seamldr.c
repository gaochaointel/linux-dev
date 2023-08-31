// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2025 Intel Corporation.
 *
 * Intel TDX module runtime update
 */
#define pr_fmt(fmt)	"seamldr: " fmt

#include <linux/cleanup.h>
#include <linux/cpuhplock.h>
#include <linux/cpumask.h>
#include <linux/irqflags.h>
#include <linux/mm.h>
#include <linux/nmi.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/stop_machine.h>
#include <linux/types.h>

#include <asm/seamldr.h>

#include "seamcall.h"
#include "tdx.h"

/* P-SEAMLDR SEAMCALL leaf function */
#define P_SEAMLDR_INFO			0x8000000000000000
#define P_SEAMLDR_INSTALL		0x8000000000000001

/* P-SEAMLDR can accept up to 496 4KB pages for TDX module binary */
#define SEAMLDR_MAX_NR_MODULE_4KB_PAGES	496

/* scenario field in struct seamldr_params */
#define SEAMLDR_SCENARIO_UPDATE		1

/*
 * Passed to P-SEAMLDR to describe information about the TDX module to install.
 * Defined in "SEAM Loader (SEAMLDR) Interface Specification", Revision
 * 343755-003, Section 3.2.
 */
struct seamldr_params {
	u32	version;
	u32	scenario;
	u64	sigstruct_pa;
	u8	reserved[104];
	u64	num_module_pages;
	u64	mod_pages_pa_list[SEAMLDR_MAX_NR_MODULE_4KB_PAGES];
} __packed;

static struct seamldr_info seamldr_info __aligned(256);
static DEFINE_RAW_SPINLOCK(seamldr_lock);

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

static void free_seamldr_params(struct seamldr_params *params)
{
	free_page((unsigned long)params);
}

/*
 * Allocate and populate a seamldr_params.
 * Note that both @module and @sig should be vmalloc'd memory.
 */
static struct seamldr_params *alloc_seamldr_params(const void *module, unsigned int module_size,
						   const void *sig, unsigned int sig_size)
{
	struct seamldr_params *params;
	const u8 *ptr;
	int i;

	BUILD_BUG_ON(sizeof(struct seamldr_params) != SZ_4K);
	if (module_size > SEAMLDR_MAX_NR_MODULE_4KB_PAGES * SZ_4K)
		return ERR_PTR(-EINVAL);

	if (!IS_ALIGNED(module_size, SZ_4K) || sig_size != SZ_4K ||
	    !IS_ALIGNED((unsigned long)module, SZ_4K) ||
	    !IS_ALIGNED((unsigned long)sig, SZ_4K))
		return ERR_PTR(-EINVAL);

	params = (struct seamldr_params *)get_zeroed_page(GFP_KERNEL);
	if (!params)
		return ERR_PTR(-ENOMEM);

	params->scenario = SEAMLDR_SCENARIO_UPDATE;

	/*
	 * Don't assume @sig is page-aligned although it is 4KB-aligned.
	 * Always add the in-page offset to get the physical address.
	 */
	params->sigstruct_pa = (vmalloc_to_pfn(sig) << PAGE_SHIFT) +
			       ((unsigned long)sig & ~PAGE_MASK);
	params->num_module_pages = module_size / SZ_4K;

	ptr = module;
	for (i = 0; i < params->num_module_pages; i++) {
		params->mod_pages_pa_list[i] = (vmalloc_to_pfn(ptr) << PAGE_SHIFT) +
					       ((unsigned long)ptr & ~PAGE_MASK);
		ptr += SZ_4K;
	}

	return params;
}

/*
 * Intel TDX Module blob. Its format is defined at:
 * https://github.com/intel/tdx-module-binaries/blob/main/blob_structure.txt
 */
struct tdx_blob {
	u16	version;
	u16	checksum;
	u32	offset_of_module;
	u8	signature[8];
	u32	len;
	u32	resv1;
	u64	resv2[509];
	u8	data[];
} __packed;

/*
 * Verify that the checksum of the entire blob is zero. The checksum is
 * calculated by summing up all 16-bit words, with carry bits dropped.
 */
static bool verify_checksum(const struct tdx_blob *blob)
{
	u32 size = blob->len;
	u16 checksum = 0;
	const u16 *p;
	int i;

	/* Handle the last byte if the size is odd */
	if (size % 2) {
		checksum += *((const u8 *)blob + size - 1);
		size--;
	}

	p = (const u16 *)blob;
	for (i = 0; i < size; i += 2) {
		checksum += *p;
		p++;
	}

	return !checksum;
}

static struct seamldr_params *init_seamldr_params(const u8 *data, u32 size)
{
	const struct tdx_blob *blob = (const void *)data;
	int module_size, sig_size;
	const void *sig, *module;

	if (blob->version != 0x100) {
		pr_err("unsupported blob version: %x\n", blob->version);
		return ERR_PTR(-EINVAL);
	}

	if (blob->resv1 || memchr_inv(blob->resv2, 0, sizeof(blob->resv2))) {
		pr_err("non-zero reserved fields\n");
		return ERR_PTR(-EINVAL);
	}

	/* Split the given blob into a sigstruct and a module */
	sig		= blob->data;
	sig_size	= blob->offset_of_module - sizeof(struct tdx_blob);
	module		= data + blob->offset_of_module;
	module_size	= size - blob->offset_of_module;

	if (sig_size <= 0 || module_size <= 0 || blob->len != size)
		return ERR_PTR(-EINVAL);

	if (memcmp(blob->signature, "TDX-BLOB", 8)) {
		pr_err("invalid signature\n");
		return ERR_PTR(-EINVAL);
	}

	if (!verify_checksum(blob)) {
		pr_err("invalid checksum\n");
		return ERR_PTR(-EINVAL);
	}

	return alloc_seamldr_params(module, module_size, sig, sig_size);
}

/*
 * During a TDX Module update, all CPUs start from TDP_START and progress
 * to TDP_DONE. Each state is associated with certain work. For some
 * states, just one CPU needs to perform the work, while other CPUs just
 * wait during those states.
 */
enum tdp_state {
	TDP_START,
	TDP_SHUTDOWN,
	TDP_CPU_INSTALL,
	TDP_DONE,
};

static struct {
	enum tdp_state state;
	atomic_t thread_ack;
	atomic_t failed;
} tdp_data;

static void set_target_state(enum tdp_state state)
{
	/* Reset ack counter. */
	atomic_set(&tdp_data.thread_ack, num_online_cpus());
	/* Ensure thread_ack is updated before the new state */
	smp_wmb();
	WRITE_ONCE(tdp_data.state, state);
}

/* Last one to ack a state moves to the next state. */
static void ack_state(void)
{
	if (atomic_dec_and_test(&tdp_data.thread_ack))
		set_target_state(tdp_data.state + 1);
}

static void print_update_failure_message(void)
{
	static atomic_t printed = ATOMIC_INIT(0);

	if (atomic_inc_return(&printed) == 1)
		pr_err("update failed, SEAMCALLs will report failure until TDs killed\n");
}

/*
 * See multi_cpu_stop() from where this multi-cpu state-machine was
 * adopted, and the rationale for touch_nmi_watchdog()
 */
static int do_seamldr_install_module(void *seamldr_params)
{
	enum tdp_state newstate, curstate = TDP_START;
	struct tdx_module_args args = {};
	int cpu = smp_processor_id();
	bool primary;
	int ret = 0;

	primary = cpumask_first(cpu_online_mask) == cpu;

	do {
		/* Chill out and ensure we re-read tdp_data. */
		cpu_relax();
		newstate = READ_ONCE(tdp_data.state);

		if (newstate != curstate) {
			curstate = newstate;
			switch (curstate) {
			case TDP_SHUTDOWN:
				if (primary)
					ret = tdx_module_shutdown();
				break;
			case TDP_CPU_INSTALL:
				args.rcx = __pa(seamldr_params);
				scoped_guard(raw_spinlock, &seamldr_lock)
					ret = seamldr_call(P_SEAMLDR_INSTALL, &args);
				break;
			default:
				break;
			}

			if (ret) {
				atomic_inc(&tdp_data.failed);
				if (curstate > TDP_SHUTDOWN)
					print_update_failure_message();
			} else {
				ack_state();
			}
		} else {
			touch_nmi_watchdog();
			rcu_momentary_eqs();
		}
	} while (curstate != TDP_DONE && !atomic_read(&tdp_data.failed));

	return ret;
}

DEFINE_FREE(free_seamldr_params, struct seamldr_params *,
	    if (!IS_ERR_OR_NULL(_T)) free_seamldr_params(_T))

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
	const struct seamldr_info *info = seamldr_get_info();
	int ret;

	if (!info)
		return -EIO;

	if (!info->num_remaining_updates)
		return -ENOSPC;

	if (!is_vmalloc_addr(data))
		return -EINVAL;

	struct seamldr_params *params __free(free_seamldr_params) =
						init_seamldr_params(data, size);
	if (IS_ERR(params))
		return PTR_ERR(params);

	guard(cpus_read_lock)();
	if (!cpumask_equal(cpu_online_mask, cpu_present_mask)) {
		pr_err("Cannot update TDX module if any CPU is offline\n");
		return -EBUSY;
	}

	atomic_set(&tdp_data.failed, 0);
	set_target_state(TDP_START + 1);
	ret = stop_machine_cpuslocked(do_seamldr_install_module, params, cpu_online_mask);
	if (ret)
		return ret;

	return 0;
}
EXPORT_SYMBOL_FOR_MODULES(seamldr_install_module, "tdx-host");
