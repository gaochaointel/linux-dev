// SPDX-License-Identifier: GPL-2.0
/*
 * TDX host user interface driver
 *
 * Copyright (C) 2025 Intel Corporation
 */

#include <linux/device/faux.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/sysfs.h>

#include <asm/cpu_device_id.h>
#include <asm/seamldr.h>
#include <asm/tdx.h>

static const struct x86_cpu_id tdx_host_ids[] = {
	X86_MATCH_FEATURE(X86_FEATURE_TDX_HOST_PLATFORM, NULL),
	{}
};
MODULE_DEVICE_TABLE(x86cpu, tdx_host_ids);

static ssize_t version_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	const struct tdx_sys_info *tdx_sysinfo = tdx_get_sysinfo();
	const struct tdx_sys_info_version *ver;

	if (!tdx_sysinfo)
		return -ENXIO;

	ver = &tdx_sysinfo->version;

	return sysfs_emit(buf, "%u.%u.%02u\n", ver->major_version,
					       ver->minor_version,
					       ver->update_version);
}
static DEVICE_ATTR_RO(version);

static struct attribute *tdx_host_attrs[] = {
	&dev_attr_version.attr,
	NULL,
};

struct attribute_group tdx_host_group = {
	.attrs = tdx_host_attrs,
};

#ifdef CONFIG_INTEL_TDX_MODULE_UPDATE
static ssize_t seamldr_version_show(struct device *dev, struct device_attribute *attr,
				    char *buf)
{
	const struct seamldr_info *info = seamldr_get_info();

	if (!info)
		return -ENXIO;

	return sysfs_emit(buf, "%u.%u.%02u\n", info->major_version,
					       info->minor_version,
					       info->update_version);
}

static ssize_t num_remaining_updates_show(struct device *dev,
					  struct device_attribute *attr,
					  char *buf)
{
	const struct seamldr_info *info = seamldr_get_info();

	if (!info)
		return -ENXIO;

	return sysfs_emit(buf, "%u\n", info->num_remaining_updates);
}

/*
 * Open-code DEVICE_ATTR_RO to specify a different 'show' function for
 * P-SEAMLDR version as version_show() is used for TDX Module version.
 */
static struct device_attribute dev_attr_seamldr_version =
	__ATTR(version, 0444, seamldr_version_show, NULL);
static DEVICE_ATTR_RO(num_remaining_updates);

static struct attribute *seamldr_attrs[] = {
	&dev_attr_seamldr_version.attr,
	&dev_attr_num_remaining_updates.attr,
	NULL,
};

static struct attribute_group seamldr_group = {
	.name = "seamldr",
	.attrs = seamldr_attrs,
};
#endif /* CONFIG_INTEL_TDX_MODULE_UPDATE */

static const struct attribute_group *tdx_host_groups[] = {
	&tdx_host_group,
#ifdef CONFIG_INTEL_TDX_MODULE_UPDATE
	&seamldr_group,
#endif
	NULL,
};

static struct faux_device *fdev;

static int __init tdx_host_init(void)
{
	if (!x86_match_cpu(tdx_host_ids) || !tdx_get_sysinfo())
		return -ENODEV;

	fdev = faux_device_create_with_groups(KBUILD_MODNAME, NULL, NULL, tdx_host_groups);
	if (!fdev)
		return -ENODEV;

	return 0;
}
module_init(tdx_host_init);

static void __exit tdx_host_exit(void)
{
	faux_device_destroy(fdev);
}
module_exit(tdx_host_exit);

MODULE_DESCRIPTION("TDX Host Services");
MODULE_LICENSE("GPL");
