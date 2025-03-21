/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2025 Intel Corporation */
#ifndef _X86_VIRT_SEAMCALL_H
#define _X86_VIRT_SEAMCALL_H

#include <linux/printk.h>
#include <linux/types.h>
#include <asm/archrandom.h>
#include <asm/tdx.h>

u64 __seamcall(u64 fn, struct tdx_module_args *args);
u64 __seamcall_ret(u64 fn, struct tdx_module_args *args);
u64 __seamcall_saved_ret(u64 fn, struct tdx_module_args *args);

typedef u64 (*sc_func_t)(u64 fn, struct tdx_module_args *args);

static inline bool is_seamldr_call(u64 fn)
{
	return fn & SEAMLDR_SEAMCALL_MASK;
}

static inline bool sc_need_retry(u64 fn, u64 error_code)
{
	if (is_seamldr_call(fn))
		return error_code == SEAMLDR_RND_NO_ENTROPY;
	else
		return error_code == TDX_RND_NO_ENTROPY;
}

static __always_inline u64 sc_retry(sc_func_t func, u64 fn,
			   struct tdx_module_args *args)
{
	int retry = RDRAND_RETRY_LOOPS;
	u64 ret;

	do {
		ret = func(fn, args);
	} while (sc_need_retry(fn, ret) && --retry);

	return ret;
}

#define seamcall(_fn, _args)		sc_retry(__seamcall, (_fn), (_args))
#define seamcall_ret(_fn, _args)	sc_retry(__seamcall_ret, (_fn), (_args))
#define seamcall_saved_ret(_fn, _args)	sc_retry(__seamcall_saved_ret, (_fn), (_args))

typedef void (*sc_err_func_t)(u64 fn, u64 err, struct tdx_module_args *args);

static inline void seamcall_err(u64 fn, u64 err, struct tdx_module_args *args)
{
	pr_err("SEAMCALL (%llu) failed: %#016llx\n", fn, err);
}

static inline void seamcall_err_ret(u64 fn, u64 err,
				    struct tdx_module_args *args)
{
	seamcall_err(fn, err, args);
	pr_err("RCX %#016llx RDX %#016llx R08 %#016llx\n",
			args->rcx, args->rdx, args->r8);
	pr_err("R09 %#016llx R10 %#016llx R11 %#016llx\n",
			args->r9, args->r10, args->r11);
}

static inline void seamldr_err(u64 fn, u64 err, struct tdx_module_args *args)
{
	/*
	 * Get the actual leaf number. No need to print the bit used to
	 * differentiate between P-SEAMLDR and TDX module as the "P-SEAMLDR"
	 * string in the error message already provides that information.
	 */
	fn &= ~SEAMLDR_SEAMCALL_MASK;
	pr_err("P-SEAMLDR (%lld) failed: 0x%016llx\n", fn, err);
}

static __always_inline int sc_retry_prerr(sc_func_t func,
					  sc_err_func_t err_func,
					  u64 fn, struct tdx_module_args *args)
{
	u64 sret = sc_retry(func, fn, args);

	if (sret == TDX_SUCCESS)
		return 0;

	if (sret == TDX_SEAMCALL_VMFAILINVALID)
		return -ENODEV;

	if (sret == TDX_SEAMCALL_GP)
		return -EOPNOTSUPP;

	if (sret == TDX_SEAMCALL_UD)
		return -EACCES;

	err_func(fn, sret, args);
	return -EIO;
}

#define seamcall_prerr(__fn, __args)						\
	sc_retry_prerr(__seamcall, seamcall_err, (__fn), (__args))

#define seamcall_prerr_ret(__fn, __args)					\
	sc_retry_prerr(__seamcall_ret, seamcall_err_ret, (__fn), (__args))

#define seamldr_prerr(__fn, __args)						\
	sc_retry_prerr(__seamcall, seamldr_err, (__fn), (__args))

#endif
