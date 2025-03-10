// SPDX-License-Identifier: GPL-2.0
/*
 * P-SEAMLDR support for TDX Module management features like runtime updates
 *
 * Copyright (C) 2025 Intel Corporation
 */
#define pr_fmt(fmt)	"seamldr: " fmt

#include <linux/cleanup.h>
#include <linux/cpuhplock.h>
#include <linux/cpumask.h>
#include <linux/mm.h>
#include <linux/nmi.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/stop_machine.h>

#include <asm/seamldr.h>

#include "seamcall_internal.h"

/* P-SEAMLDR SEAMCALL leaf function */
#define P_SEAMLDR_INFO			0x8000000000000000

#define SEAMLDR_MAX_NR_MODULE_4KB_PAGES	496
#define SEAMLDR_MAX_NR_SIG_4KB_PAGES	4

/*
 * The seamldr_params "scenario" field specifies the operation mode:
 * 0: Install TDX Module from scratch (not used by kernel)
 * 1: Update existing TDX Module to a compatible version
 */
#define SEAMLDR_SCENARIO_UPDATE		1

/*
 * This is called the "SEAMLDR_PARAMS" data structure and is defined
 * in "SEAM Loader (SEAMLDR) Interface Specification".
 *
 * It describes the TDX Module that will be installed.
 */
struct seamldr_params {
	u32	version;
	u32	scenario;
	u64	sigstruct_pa[SEAMLDR_MAX_NR_SIG_4KB_PAGES];
	u8	reserved[80];
	u64	num_module_pages;
	u64	mod_pages_pa_list[SEAMLDR_MAX_NR_MODULE_4KB_PAGES];
} __packed;

static_assert(sizeof(struct seamldr_params) == 4096);

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

static void free_seamldr_params(struct seamldr_params *params)
{
	free_page((unsigned long)params);
}

static struct seamldr_params *alloc_seamldr_params(const void *module, unsigned int module_size,
						   const void *sig, unsigned int sig_size)
{
	struct seamldr_params *params;
	const u8 *ptr;
	int i;

	if (WARN_ON_ONCE(!is_vmalloc_addr(module) || !is_vmalloc_addr(sig)))
		return ERR_PTR(-EINVAL);

	if (module_size > SEAMLDR_MAX_NR_MODULE_4KB_PAGES * SZ_4K)
		return ERR_PTR(-EINVAL);

	if (sig_size > SEAMLDR_MAX_NR_SIG_4KB_PAGES * SZ_4K)
		return ERR_PTR(-EINVAL);

	/*
	 * Check that input buffers satisfy P-SEAMLDR's size and alignment
	 * constraints so they can be passed directly to P-SEAMLDR without
	 * relocation or copy.
	 */
	if (!IS_ALIGNED(module_size, SZ_4K) || !IS_ALIGNED(sig_size, SZ_4K) ||
	    !IS_ALIGNED((unsigned long)module, SZ_4K) ||
	    !IS_ALIGNED((unsigned long)sig, SZ_4K))
		return ERR_PTR(-EINVAL);

	params = (struct seamldr_params *)get_zeroed_page(GFP_KERNEL);
	if (!params)
		return ERR_PTR(-ENOMEM);

	params->scenario = SEAMLDR_SCENARIO_UPDATE;

	ptr = sig;
	for (i = 0; i < sig_size / SZ_4K; i++) {
		/*
		 * Don't assume @sig is page-aligned although it is 4KB-aligned.
		 * Always add the in-page offset to get the physical address.
		 */
		params->sigstruct_pa[i] = (vmalloc_to_pfn(ptr) << PAGE_SHIFT) +
					  ((unsigned long)ptr & ~PAGE_MASK);
		ptr += SZ_4K;
	}

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
 *
 * Note this structure differs from the reference above: the two variable-length
 * fields "@sigstruct" and "@module" are represented as a single "@data" field
 * here and split programmatically using the offset_of_module value.
 */
struct tdx_blob {
	u16	version;
	u16	checksum;
	u32	offset_of_module;
	u8	signature[8];
	u32	length;
	u32	resv0;
	u64	resv1[509];
	u8	data[];
} __packed;

static struct seamldr_params *init_seamldr_params(const u8 *data, u32 size)
{
	const struct tdx_blob *blob = (const void *)data;
	int module_size, sig_size;
	const void *sig, *module;

	if (size < sizeof(struct tdx_blob) || blob->offset_of_module >= size)
		return ERR_PTR(-EINVAL);

	if (blob->version != 0x100) {
		pr_err("unsupported blob version: %x\n", blob->version);
		return ERR_PTR(-EINVAL);
	}

	if (blob->resv0 || memchr_inv(blob->resv1, 0, sizeof(blob->resv1))) {
		pr_err("non-zero reserved fields\n");
		return ERR_PTR(-EINVAL);
	}

	/* Split the blob into a sigstruct and a module */
	sig		= blob->data;
	sig_size	= blob->offset_of_module - sizeof(struct tdx_blob);
	module		= data + blob->offset_of_module;
	module_size	= size - blob->offset_of_module;

	if (sig_size <= 0 || module_size <= 0 || blob->length != size)
		return ERR_PTR(-EINVAL);

	if (memcmp(blob->signature, "TDX-BLOB", 8)) {
		pr_err("invalid signature\n");
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
	TDP_DONE,
};

static struct {
	enum tdp_state state;
	atomic_t thread_ack;
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

/*
 * See multi_cpu_stop() from where this multi-cpu state-machine was
 * adopted, and the rationale for touch_nmi_watchdog()
 */
static int do_seamldr_install_module(void *params)
{
	enum tdp_state newstate, curstate = TDP_START;
	int ret = 0;

	do {
		/* Chill out and re-read tdp_data */
		cpu_relax();
		newstate = READ_ONCE(tdp_data.state);

		if (newstate != curstate) {
			curstate = newstate;
			switch (curstate) {
			default:
				break;
			}
			ack_state();
		} else {
			touch_nmi_watchdog();
			rcu_momentary_eqs();
		}
	} while (curstate != TDP_DONE);

	return ret;
}

DEFINE_FREE(free_seamldr_params, struct seamldr_params *,
	    if (!IS_ERR_OR_NULL(_T)) free_seamldr_params(_T))

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
	struct seamldr_info info;
	int ret;

	ret = seamldr_get_info(&info);
	if (ret)
		return ret;

	if (!info.num_remaining_updates)
		return -ENOSPC;

	if (WARN_ON_ONCE(!is_vmalloc_addr(data)))
		return -EINVAL;

	struct seamldr_params *params __free(free_seamldr_params) =
						init_seamldr_params(data, size);
	if (IS_ERR(params))
		return PTR_ERR(params);

	guard(cpus_read_lock)();
	if (!cpumask_equal(cpu_online_mask, cpu_present_mask)) {
		pr_err("Cannot update the TDX Module if any CPU is offline\n");
		return -EBUSY;
	}

	set_target_state(TDP_START + 1);
	ret = stop_machine_cpuslocked(do_seamldr_install_module, params, cpu_online_mask);
	if (ret)
		return ret;

	return 0;
}
EXPORT_SYMBOL_FOR_MODULES(seamldr_install_module, "tdx-host");
