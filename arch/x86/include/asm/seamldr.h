/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_SEAMLDR_H
#define _ASM_X86_SEAMLDR_H

#include <linux/types.h>

struct seamldr_info {
	u32	version;
	u32	attributes;
	u32	vendor_id;
	u32	build_date;
	u16	build_num;
	u16	minor_version;
	u16	major_version;
	u16	update_version;
	u8	reserved0[4];
	u32	num_remaining_updates;
	u8	reserved1[224];
} __packed;

#ifdef CONFIG_INTEL_TDX_MODULE_UPDATE
const struct seamldr_info *seamldr_get_info(void);
int seamldr_install_module(const u8 *data, u32 size);
#else
static inline const struct seamldr_info *seamldr_get_info(void) { return NULL; }
static inline int seamldr_install_module(const u8 *data, u32 size) { return -EOPNOTSUPP; }
#endif

#endif
