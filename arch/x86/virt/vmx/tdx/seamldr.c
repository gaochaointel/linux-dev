// SPDX-License-Identifier: GPL-2.0
/*
 * P-SEAMLDR support for TDX module management features like runtime updates
 *
 * Copyright (C) 2025 Intel Corporation
 */
#define pr_fmt(fmt)	"seamldr: " fmt

#include <linux/mm.h>
#include <linux/nmi.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/stop_machine.h>

#include <asm/seamldr.h>

#include "seamcall_internal.h"
#include "tdx.h"

/* P-SEAMLDR SEAMCALL leaf function */
#define P_SEAMLDR_INFO			0x8000000000000000

#define SEAMLDR_MAX_NR_MODULE_4KB_PAGES	496
#define SEAMLDR_MAX_NR_SIG_4KB_PAGES	4

/*
 * The seamldr_params "scenario" field specifies the operation mode:
 * 0: Install TDX module from scratch (not used by kernel)
 * 1: Update existing TDX module to a compatible version
 */
#define SEAMLDR_SCENARIO_UPDATE		1

/*
 * This is called the "SEAMLDR_PARAMS" data structure and is defined
 * in "SEAM Loader (SEAMLDR) Interface Specification".
 *
 * It describes the TDX module that will be installed.
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
 * interact with P-SEAMLDR simultaneously. Use raw version as the calls can
 * be made with interrupts disabled.
 */
static DEFINE_RAW_SPINLOCK(seamldr_lock);

static int seamldr_call(u64 fn, struct tdx_module_args *args)
{
	guard(raw_spinlock)(&seamldr_lock);
	return seamcall_prerr(fn, args);
}

int seamldr_get_info(struct seamldr_info *seamldr_info)
{
	/*
	 * Use slow_virt_to_phys() since @seamldr_info may be allocated on
	 * the stack.
	 */
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

	if (module_size > SEAMLDR_MAX_NR_MODULE_4KB_PAGES * SZ_4K)
		return ERR_PTR(-EINVAL);

	if (sig_size > SEAMLDR_MAX_NR_SIG_4KB_PAGES * SZ_4K)
		return ERR_PTR(-EINVAL);

	params = (struct seamldr_params *)get_zeroed_page(GFP_KERNEL);
	if (!params)
		return ERR_PTR(-ENOMEM);

	/*
	 * Only use version 1 when required (sigstruct > 4KB) for backward
	 * compatibility with P-SEAMLDR that lacks version 1 support.
	 */
	if (sig_size > SZ_4K)
		params->version = 1;
	else
		params->version = 0;

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
 * Intel TDX module blob. Its format is defined at:
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
	u32	reserved0;
	u64	reserved1[509];
	u8	data[];
} __packed;

/* Supported versions of the tdx_blob */
#define TDX_BLOB_VERSION_1	0x100

static struct seamldr_params *init_seamldr_params(const u8 *data, u32 size)
{
	const struct tdx_blob *blob = (const void *)data;
	int module_size, sig_size;
	const void *sig, *module;

	/* Ensure the size is valid otherwise reading any field from the blob may overflow. */
	if (size <= sizeof(struct tdx_blob) || size <= blob->offset_of_module)
		return ERR_PTR(-EINVAL);

	if (blob->version != TDX_BLOB_VERSION_1) {
		pr_err("unsupported blob version: %x\n", blob->version);
		return ERR_PTR(-EINVAL);
	}

	/* Split the blob into a sigstruct and a module. */
	sig		= blob->data;
	sig_size	= blob->offset_of_module - sizeof(struct tdx_blob);
	module		= data + blob->offset_of_module;
	module_size	= size - blob->offset_of_module;

	if (sig_size <= 0 || module_size <= 0 || blob->length != size)
		return ERR_PTR(-EINVAL);

	return alloc_seamldr_params(module, module_size, sig, sig_size);
}

/*
 * During a TDX module update, all CPUs start from MODULE_UPDATE_START and
 * progress to MODULE_UPDATE_DONE. Each state is associated with certain
 * work. For some states, just one CPU needs to perform the work, while
 * other CPUs just wait during those states.
 */
enum module_update_state {
	MODULE_UPDATE_START,
	MODULE_UPDATE_SHUTDOWN,
	MODULE_UPDATE_DONE,
};

static struct {
	enum module_update_state state;
	int thread_ack;
	int failed;
	/*
	 * Protect update_data. Raw spinlock as it will be acquired from
	 * interrupt-disabled contexts.
	 */
	raw_spinlock_t lock;
} update_data = {
	.lock = __RAW_SPIN_LOCK_UNLOCKED(update_data.lock)
};

static void set_target_state(enum module_update_state state)
{
	/* Reset ack counter. */
	update_data.thread_ack = num_online_cpus();
	update_data.state = state;
}

/* Last one to ack a state moves to the next state. */
static void ack_state(void)
{
	guard(raw_spinlock)(&update_data.lock);
	update_data.thread_ack--;
	if (!update_data.thread_ack)
		set_target_state(update_data.state + 1);
}

/*
 * See multi_cpu_stop() from where this multi-cpu state-machine was
 * adopted, and the rationale for touch_nmi_watchdog().
 */
static int do_seamldr_install_module(void *seamldr_params)
{
	enum module_update_state newstate, curstate = MODULE_UPDATE_START;
	int cpu = smp_processor_id();
	bool primary;
	int ret = 0;

	primary = cpumask_first(cpu_online_mask) == cpu;

	do {
		/* Chill out and re-read update_data. */
		cpu_relax();
		newstate = READ_ONCE(update_data.state);

		if (newstate != curstate) {
			curstate = newstate;
			switch (curstate) {
			case MODULE_UPDATE_SHUTDOWN:
				if (primary)
					ret = tdx_module_shutdown();
				break;
			default:
				break;
			}

			if (ret) {
				scoped_guard(raw_spinlock, &update_data.lock)
					update_data.failed++;
			} else {
				ack_state();
			}
		} else {
			touch_nmi_watchdog();
			rcu_momentary_eqs();
		}
	} while (curstate != MODULE_UPDATE_DONE && !READ_ONCE(update_data.failed));

	return ret;
}

DEFINE_FREE(free_seamldr_params, struct seamldr_params *,
	    if (!IS_ERR_OR_NULL(_T)) free_seamldr_params(_T))

/**
 * seamldr_install_module - Install a new TDX module.
 * @data: Pointer to the TDX module update blob.
 * @size: Size of the TDX module update blob.
 *
 * Returns 0 on success, negative error code on failure.
 */
int seamldr_install_module(const u8 *data, u32 size)
{
	struct seamldr_params *params __free(free_seamldr_params) =
						init_seamldr_params(data, size);
	if (IS_ERR(params))
		return PTR_ERR(params);

	update_data.failed = 0;
	set_target_state(MODULE_UPDATE_START + 1);
	return stop_machine(do_seamldr_install_module, params, cpu_online_mask);
}
EXPORT_SYMBOL_FOR_MODULES(seamldr_install_module, "tdx-host");
