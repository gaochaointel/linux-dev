/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_TDX_GLOBAL_METADATA_H
#define _ASM_X86_TDX_GLOBAL_METADATA_H

#include <linux/types.h>

/*
 * TDX module "Global Scope Metadata" as documented in the Intel TDX
 * Module ABI spec. Each sub-structure below corresponds to one TDX
 * metadata "Class"; its members are populated via TDH.SYS.RD SEAMCALLs.
 */

struct tdx_sys_info_version {
	u16 minor_version;
	u16 major_version;
	u16 update_version;
};

struct tdx_sys_info_features {
	u64 tdx_features0;
};

struct tdx_sys_info_tdmr {
	u16 max_tdmrs;
	u16 max_reserved_per_tdmr;
	u16 pamt_4k_entry_size;
	u16 pamt_2m_entry_size;
	u16 pamt_1g_entry_size;
};

struct tdx_sys_info_td_ctrl {
	u16 tdr_base_size;
	u16 tdcs_base_size;
	u16 tdvps_base_size;
};

struct tdx_sys_info_td_conf {
	u64 attributes_fixed0;
	u64 attributes_fixed1;
	u64 xfam_fixed0;
	u64 xfam_fixed1;
	u16 num_cpuid_config;
	u16 max_vcpus_per_td;
	u64 cpuid_config_leaves[128];
	u64 cpuid_config_values[128][2];
};

struct tdx_sys_info_handoff {
	u16 module_hv;
};

struct tdx_sys_info {
	struct tdx_sys_info_version version;
	struct tdx_sys_info_features features;
	struct tdx_sys_info_tdmr tdmr;
	struct tdx_sys_info_td_ctrl td_ctrl;
	struct tdx_sys_info_td_conf td_conf;
};

#endif /* _ASM_X86_TDX_GLOBAL_METADATA_H */
