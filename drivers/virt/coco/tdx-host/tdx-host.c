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
#include <linux/firmware.h>
#include <asm/cpu_device_id.h>
#include <asm/seamldr.h>
#include <asm/tdx.h>
#include <asm/tdx_global_metadata.h>

static const struct x86_cpu_id tdx_host_ids[] = {
	X86_MATCH_FEATURE(X86_FEATURE_TDX_HOST_PLATFORM, NULL),
	{}
};
MODULE_DEVICE_TABLE(x86cpu, tdx_host_ids);

struct tdx_fw_upload_status {
	bool cancel_request;
};

struct fw_upload *tdx_fwl;
static struct tdx_fw_upload_status tdx_fw_upload_status;

static struct faux_device *fdev;

static ssize_t version_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	const struct tdx_sys_info *tdx_sysinfo = tdx_get_sysinfo();
	const struct tdx_sys_info_version *ver;

	/*
	 * Inform userspace that the TDX module isn't in a usable state,
	 * possibly due to a failed update.
	 */
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

static enum fw_upload_err tdx_fw_prepare(struct fw_upload *fwl,
					 const u8 *data, u32 size)
{
	struct tdx_fw_upload_status *status = fwl->dd_handle;

	if (status->cancel_request) {
		status->cancel_request = false;
		return FW_UPLOAD_ERR_CANCELED;
	}

	return FW_UPLOAD_ERR_NONE;
}

static enum fw_upload_err tdx_fw_write(struct fw_upload *fwl, const u8 *data,
				       u32 offset, u32 size, u32 *written)
{
	struct tdx_fw_upload_status *status = fwl->dd_handle;

	if (status->cancel_request) {
		status->cancel_request = false;
		return FW_UPLOAD_ERR_CANCELED;
	}

	/*
	 * tdx_fw_write() always processes all data on the first call with
	 * offset == 0. Since it never returns partial success (it either
	 * succeeds completely or fails), there is no subsequent call with
	 * non-zero offsets.
	 */
	WARN_ON_ONCE(offset);
	if (seamldr_install_module(data, size))
		return FW_UPLOAD_ERR_FW_INVALID;

	*written = size;
	return FW_UPLOAD_ERR_NONE;
}

static enum fw_upload_err tdx_fw_poll_complete(struct fw_upload *fwl)
{
	/*
	 * TDX Module updates are completed in the previous phase
	 * (tdx_fw_write()). If any error occurred, the previous phase
	 * would return an error code to abort the update process. In
	 * other words, reaching this point means the update succeeded.
	 */
	return FW_UPLOAD_ERR_NONE;
}

static void tdx_fw_cancel(struct fw_upload *fwl)
{
	struct tdx_fw_upload_status *status = fwl->dd_handle;

	status->cancel_request = true;
}

static const struct fw_upload_ops tdx_fw_ops = {
	.prepare = tdx_fw_prepare,
	.write = tdx_fw_write,
	.poll_complete = tdx_fw_poll_complete,
	.cancel = tdx_fw_cancel,
};

static int seamldr_init(struct device *dev)
{
	const struct seamldr_info *seamldr_info = seamldr_get_info();
	const struct tdx_sys_info *tdx_sysinfo = tdx_get_sysinfo();
	int ret;

	if (!tdx_sysinfo || !seamldr_info)
		return -ENXIO;

	if (!tdx_supports_runtime_update(tdx_sysinfo)) {
		pr_info("Current TDX Module cannot be updated. Consider BIOS updates\n");
		return -EOPNOTSUPP;
	}

	if (!seamldr_info->num_remaining_updates) {
		pr_info("P-SEAMLDR doesn't support TDX Module updates\n");
		return -EOPNOTSUPP;
	}

	tdx_fwl = firmware_upload_register(THIS_MODULE, dev, "seamldr_upload",
					   &tdx_fw_ops, &tdx_fw_upload_status);
	ret = PTR_ERR_OR_ZERO(tdx_fwl);
	if (ret)
		pr_err("failed to register module uploader %d\n", ret);

	return ret;
}

static void seamldr_deinit(void)
{
	if (tdx_fwl)
		firmware_upload_unregister(tdx_fwl);
}

static int tdx_host_probe(struct faux_device *fdev)
{
	/* Only support TDX Module updates now. More TDX features could be added here. */
	return seamldr_init(&fdev->dev);
}

static void tdx_host_remove(struct faux_device *fdev)
{
	seamldr_deinit();
}

static struct faux_device_ops tdx_host_ops = {
	.probe		= tdx_host_probe,
	.remove		= tdx_host_remove,
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

	fdev = faux_device_create_with_groups(KBUILD_MODNAME, NULL, &tdx_host_ops, tdx_host_groups);
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
