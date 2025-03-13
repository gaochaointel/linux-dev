// SPDX-License-Identifier: GPL-2.0
/*
 * TDX host user interface driver
 *
 * Copyright (C) 2025 Intel Corporation
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/sysfs.h>
#include <linux/device/faux.h>
#include <asm/cpu_device_id.h>
#include <asm/seamldr.h>
#include <asm/tdx.h>
#include <asm/tdx_global_metadata.h>

static const struct x86_cpu_id tdx_host_ids[] = {
	X86_MATCH_FEATURE(X86_FEATURE_TDX_HOST_PLATFORM, NULL),
	{}
};
MODULE_DEVICE_TABLE(x86cpu, tdx_host_ids);

static struct faux_device *fdev;

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

static ssize_t seamldr_version_show(struct device *dev, struct device_attribute *attr,
				    char *buf)
{
	const struct seamldr_info *info = seamldr_get_info();

	if (!info)
		return -ENXIO;

	return sysfs_emit(buf, "%u.%u.%u\n", info->major_version,
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
 * P-SEAMLDR version as version_show() is used for the TDX Module version.
 */
static struct device_attribute dev_attr_seamldr_version =
	__ATTR(version, 0444, seamldr_version_show, NULL);
static DEVICE_ATTR_RO(num_remaining_updates);

static struct attribute *seamldr_attrs[] = {
	&dev_attr_seamldr_version.attr,
	&dev_attr_num_remaining_updates.attr,
	NULL,
};

static umode_t seamldr_group_is_visible(struct kobject *kobj,
					struct attribute *attr, int n)
{
	return seamldr_get_info() ? attr->mode : 0;
}

static struct attribute_group seamldr_group = {
	.name = "seamldr",
	.attrs = seamldr_attrs,
	.is_visible = seamldr_group_is_visible,
};

static const struct attribute_group *tdx_host_groups[] = {
	&tdx_host_group,
	&seamldr_group,
	NULL,
};

static int __init tdx_host_init(void)
{
	int r;

	if (!x86_match_cpu(tdx_host_ids))
		return -ENODEV;

	/* Enable the usage of SEAMCALLs */
	r = tdx_enable();
	if (r)
		return r;

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
