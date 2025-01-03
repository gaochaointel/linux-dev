/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __KVM_HARDWARE_ENABLE_H
#define __KVM_HARDWARE_ENABLE_H

#include <linux/types.h>

#ifdef CONFIG_KVM_GENERIC_HARDWARE_ENABLING
extern bool enable_virt_at_load;
extern bool kvm_rebooting;

/*
 * kvm_arch_{enable,disable}_virtualization() are called on one CPU, under
 * kvm_usage_lock, immediately after/before 0=>1 and 1=>0 transitions of
 * kvm_usage_count, i.e. at the beginning of the generic hardware enabling
 * sequence, and at the end of the generic hardware disabling sequence.
 */
int kvm_arch_enable_virtualization(void);
void kvm_arch_disable_virtualization(void);
/*
 * kvm_arch_{enable,disable}_virtualization_cpu() are called on "every" CPU to
 * do the actual twiddling of hardware bits.  The hooks are called on all
 * online CPUs when KVM enables/disabled virtualization, and on a single CPU
 * when that CPU is onlined/offlined (including for Resume/Suspend).
 */
int kvm_arch_enable_virtualization_cpu(void);
void kvm_arch_disable_virtualization_cpu(void);
int kvm_enable_virtualization(void);
void kvm_disable_virtualization(void);
int kvm_init_virtualization(void);
void kvm_uninit_virtualization(void);
#else
static inline int kvm_enable_virtualization(void) { return 0; }
static inline void kvm_disable_virtualization(void) { }
static inline int kvm_init_virtualization(void) { return 0; }
static inline void kvm_uninit_virtualization(void) { }
#endif

#endif
