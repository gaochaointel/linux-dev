// SPDX-License-Identifier: GPL-2.0-only
#include <linux/types.h>
#include <linux/module.h>
#include <linux/percpu-defs.h>
#include <linux/mutex.h>
#include <linux/syscore_ops.h>
#include <linux/cpuhotplug.h>
#include <linux/kvm_hardware_enable.h>

bool enable_virt_at_load = true;
module_param(enable_virt_at_load, bool, 0444);
EXPORT_SYMBOL_GPL(enable_virt_at_load);

__visible bool kvm_rebooting;
EXPORT_SYMBOL_GPL(kvm_rebooting);

static DEFINE_PER_CPU(bool, virtualization_enabled);
static DEFINE_MUTEX(kvm_usage_lock);
static int kvm_usage_count;

__weak void kvm_arch_enable_virtualization(void)
{

}

__weak void kvm_arch_disable_virtualization(void)
{

}

static int kvm_enable_virtualization_cpu(void)
{
	if (__this_cpu_read(virtualization_enabled))
		return 0;

	if (kvm_arch_enable_virtualization_cpu()) {
		pr_info("kvm: enabling virtualization on CPU%d failed\n",
			raw_smp_processor_id());
		return -EIO;
	}

	__this_cpu_write(virtualization_enabled, true);
	return 0;
}

static int kvm_online_cpu(unsigned int cpu)
{
	/*
	 * Abort the CPU online process if hardware virtualization cannot
	 * be enabled. Otherwise running VMs would encounter unrecoverable
	 * errors when scheduled to this CPU.
	 */
	return kvm_enable_virtualization_cpu();
}

static void kvm_disable_virtualization_cpu(void *ign)
{
	if (!__this_cpu_read(virtualization_enabled))
		return;

	kvm_arch_disable_virtualization_cpu();

	__this_cpu_write(virtualization_enabled, false);
}

static int kvm_offline_cpu(unsigned int cpu)
{
	kvm_disable_virtualization_cpu(NULL);
	return 0;
}

static void kvm_shutdown(void)
{
	/*
	 * Disable hardware virtualization and set kvm_rebooting to indicate
	 * that KVM has asynchronously disabled hardware virtualization, i.e.
	 * that relevant errors and exceptions aren't entirely unexpected.
	 * Some flavors of hardware virtualization need to be disabled before
	 * transferring control to firmware (to perform shutdown/reboot), e.g.
	 * on x86, virtualization can block INIT interrupts, which are used by
	 * firmware to pull APs back under firmware control.  Note, this path
	 * is used for both shutdown and reboot scenarios, i.e. neither name is
	 * 100% comprehensive.
	 */
	pr_info("kvm: exiting hardware virtualization\n");
	kvm_rebooting = true;
	on_each_cpu(kvm_disable_virtualization_cpu, NULL, 1);
}

static int kvm_suspend(void)
{
	/*
	 * Secondary CPUs and CPU hotplug are disabled across the suspend/resume
	 * callbacks, i.e. no need to acquire kvm_usage_lock to ensure the usage
	 * count is stable.  Assert that kvm_usage_lock is not held to ensure
	 * the system isn't suspended while KVM is enabling hardware.  Hardware
	 * enabling can be preempted, but the task cannot be frozen until it has
	 * dropped all locks (userspace tasks are frozen via a fake signal).
	 */
	lockdep_assert_not_held(&kvm_usage_lock);
	lockdep_assert_irqs_disabled();

	kvm_disable_virtualization_cpu(NULL);
	return 0;
}

static void kvm_resume(void)
{
	lockdep_assert_not_held(&kvm_usage_lock);
	lockdep_assert_irqs_disabled();

	WARN_ON_ONCE(kvm_enable_virtualization_cpu());
}

static struct syscore_ops kvm_syscore_ops = {
	.suspend = kvm_suspend,
	.resume = kvm_resume,
	.shutdown = kvm_shutdown,
};

int kvm_enable_virtualization(void)
{
	int r;

	guard(mutex)(&kvm_usage_lock);

	if (kvm_usage_count++)
		return 0;

	kvm_arch_enable_virtualization();

	r = cpuhp_setup_state(CPUHP_AP_KVM_ONLINE, "kvm/cpu:online",
			      kvm_online_cpu, kvm_offline_cpu);
	if (r)
		goto err_cpuhp;

	register_syscore_ops(&kvm_syscore_ops);

	/*
	 * Undo virtualization enabling and bail if the system is going down.
	 * If userspace initiated a forced reboot, e.g. reboot -f, then it's
	 * possible for an in-flight operation to enable virtualization after
	 * syscore_shutdown() is called, i.e. without kvm_shutdown() being
	 * invoked.  Note, this relies on system_state being set _before_
	 * kvm_shutdown(), e.g. to ensure either kvm_shutdown() is invoked
	 * or this CPU observes the impending shutdown.  Which is why KVM uses
	 * a syscore ops hook instead of registering a dedicated reboot
	 * notifier (the latter runs before system_state is updated).
	 */
	if (system_state == SYSTEM_HALT || system_state == SYSTEM_POWER_OFF ||
	    system_state == SYSTEM_RESTART) {
		r = -EBUSY;
		goto err_rebooting;
	}

	return 0;

err_rebooting:
	unregister_syscore_ops(&kvm_syscore_ops);
	cpuhp_remove_state(CPUHP_AP_KVM_ONLINE);
err_cpuhp:
	kvm_arch_disable_virtualization();
	--kvm_usage_count;
	return r;
}
EXPORT_SYMBOL_GPL(kvm_enable_virtualization);

void kvm_disable_virtualization(void)
{
	guard(mutex)(&kvm_usage_lock);

	if (--kvm_usage_count)
		return;

	unregister_syscore_ops(&kvm_syscore_ops);
	cpuhp_remove_state(CPUHP_AP_KVM_ONLINE);
	kvm_arch_disable_virtualization();
}
EXPORT_SYMBOL_GPL(kvm_disable_virtualization);

int kvm_init_virtualization(void)
{
	if (enable_virt_at_load)
		return kvm_enable_virtualization();

	return 0;
}

void kvm_uninit_virtualization(void)
{
	if (enable_virt_at_load)
		kvm_disable_virtualization();
}
