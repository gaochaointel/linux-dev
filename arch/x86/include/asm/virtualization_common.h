// SPDX-License-Identifier: GPL-2.0-only
#ifndef ARCH_X86_VIRTUALIZATION_COMMON_H
#define ARCH_X86_VIRTUALIZATION_COMMON_H

#include <linux/percpu-defs.h>
#include <linux/preempt.h>

#include <asm/cpuid.h>
#include <asm/processor.h>
#include <asm/tlbflush.h>

static inline int cpu_vmxon(u64 vmxon_pointer)
{
	u64 msr;

	cr4_set_bits(X86_CR4_VMXE);

	asm goto("1: vmxon %[vmxon_pointer]\n\t"
			  _ASM_EXTABLE(1b, %l[fault])
			  : : [vmxon_pointer] "m"(vmxon_pointer)
			  : : fault);
	return 0;

fault:
	WARN_ONCE(1, "VMXON faulted, MSR_IA32_FEAT_CTL (0x3a) = 0x%llx\n",
		  rdmsrq_safe(MSR_IA32_FEAT_CTL, &msr) ? 0xdeadbeef : msr);
	cr4_clear_bits(X86_CR4_VMXE);

	return -EFAULT;
}

/*
 * Disable VMX and clear CR4.VMXE (even if VMXOFF faults)
 *
 * Note, VMXOFF causes a #UD if the CPU is !post-VMXON, but it's impossible to
 * atomically track post-VMXON state, e.g. this may be called in NMI context.
 * Eat all faults as all other faults on VMXOFF faults are mode related, i.e.
 * faults are guaranteed to be due to the !post-VMXON check unless the CPU is
 * magically in RM, VM86, compat mode, or at CPL>0.
 */
static inline int cpu_vmxoff(void)
{
	asm goto("1: vmxoff\n\t"
			  _ASM_EXTABLE(1b, %l[fault])
			  ::: "cc", "memory" : fault);

	cr4_clear_bits(X86_CR4_VMXE);
	return 0;

fault:
	cr4_clear_bits(X86_CR4_VMXE);
	return -EIO;
}

static inline bool __is_vmx_supported(void)
{
	int cpu = smp_processor_id();

	if (!(cpuid_ecx(1) & BIT(5))) {
		pr_err("VMX not supported by CPU %d\n", cpu);
		return false;
	}

	if (!this_cpu_has(X86_FEATURE_MSR_IA32_FEAT_CTL) ||
	    !this_cpu_has(X86_FEATURE_VMX)) {
		pr_err("VMX not enabled (by BIOS) in MSR_IA32_FEAT_CTL on CPU %d\n", cpu);
		return false;
	}

	return true;
}

static inline bool is_vmx_supported(void)
{
	bool supported;

	migrate_disable();
	supported = __is_vmx_supported();
	migrate_enable();

	return supported;
}

#endif
