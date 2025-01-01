// SPDX-License-Identifier: GPL-2.0-only

#include <linux/smp.h>
#include <linux/percpu-defs.h>
#include <linux/kvm_hardware_enable.h>
#include <asm/page.h>
#include <asm/perf_event.h>
#include <asm/vmx.h>
#include <asm/virtualization_common.h>

#include "x86.h"

DEFINE_PER_CPU(struct vmcs *, vmxarea);
EXPORT_PER_CPU_SYMBOL_GPL(vmxarea);

void free_kvm_area(void)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		free_page((unsigned long)per_cpu(vmxarea, cpu));
		per_cpu(vmxarea, cpu) = NULL;
	}
}

static struct vmcs *__alloc_vmcs_cpu(int cpu, gfp_t flags)
{
	int node = cpu_to_node(cpu);
	struct page *pages;
	struct vmcs *vmcs;
	u64 basic;

	rdmsrl(MSR_IA32_VMX_BASIC, basic);

	pages = __alloc_pages_node(node, flags, 0);
	if (!pages)
		return NULL;
	vmcs = page_address(pages);
	memset(vmcs, 0, vmx_basic_vmcs_size(basic));

	vmcs->hdr.revision_id = vmx_basic_vmcs_revision_id(basic);

	return vmcs;
}

int alloc_kvm_area(void)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		struct vmcs *vmcs;

		vmcs = __alloc_vmcs_cpu(cpu, GFP_KERNEL);
		if (!vmcs) {
			free_kvm_area();
			return -ENOMEM;
		}

		per_cpu(vmxarea, cpu) = vmcs;
	}
	return 0;
}
