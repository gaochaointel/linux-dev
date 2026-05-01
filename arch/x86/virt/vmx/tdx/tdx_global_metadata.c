// SPDX-License-Identifier: GPL-2.0
/*
 * Automatically generated functions to read TDX global metadata.
 *
 * This file doesn't compile on its own as it lacks of inclusion
 * of SEAMCALL wrapper primitive which reads global metadata.
 * Include this file to other C file instead.
 */

static __init int get_tdx_sys_info(struct tdx_sys_info *sysinfo)
{
	int ret = 0;

	ret = ret ?: get_tdx_sys_info_version(&sysinfo->version);

	pr_info("Module version: " TDX_VERSION_FMT "\n",
		sysinfo->version.major_version,
		sysinfo->version.minor_version,
		sysinfo->version.update_version);

	ret = ret ?: get_tdx_sys_info_features(&sysinfo->features);
	ret = ret ?: get_tdx_sys_info_tdmr(&sysinfo->tdmr);
	ret = ret ?: get_tdx_sys_info_td_ctrl(&sysinfo->td_ctrl);
	ret = ret ?: get_tdx_sys_info_td_conf(&sysinfo->td_conf);

	return ret;
}
