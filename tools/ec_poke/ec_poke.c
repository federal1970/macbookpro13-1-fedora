// SPDX-License-Identifier: GPL-2.0
/*
 * ec_poke - read and write single bytes of the ACPI Embedded Controller's RAM
 * through sysfs, using the kernel's own ec_read()/ec_write() (the same calls
 * the ACPI interpreter makes for EmbeddedControl operation regions).
 *
 * Stand-in for the in-tree ec_sys debugfs module, which Fedora does not build
 * (CONFIG_ACPI_EC_DEBUGFS is not set). Made for one experiment on a
 * MacBookPro13,1: the EC's wake-enable bank at 0x64..0x6d.
 *
 *   /sys/kernel/ec_poke/addr   write "69"  (hex byte address)
 *   /sys/kernel/ec_poke/data   read  -> hex byte at addr
 *                              write "08" -> writes that byte at addr
 *   /sys/kernel/ec_poke/dump   read  -> 256 bytes, 16 per line
 *
 * Everything is root-only. There is no range check beyond the byte address
 * itself: this is a debugging tool, not a driver.
 */
#include <linux/module.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/acpi.h>
#include <linux/hex.h>

static struct kobject *ec_poke_kobj;
static u8 ec_poke_addr;

static ssize_t addr_show(struct kobject *kobj, struct kobj_attribute *attr,
			 char *buf)
{
	return sysfs_emit(buf, "%02x\n", ec_poke_addr);
}

static ssize_t addr_store(struct kobject *kobj, struct kobj_attribute *attr,
			  const char *buf, size_t count)
{
	u8 v;

	if (kstrtou8(buf, 16, &v))
		return -EINVAL;
	ec_poke_addr = v;
	return count;
}

static ssize_t data_show(struct kobject *kobj, struct kobj_attribute *attr,
			 char *buf)
{
	u8 v;
	int ret;

	ret = ec_read(ec_poke_addr, &v);
	if (ret)
		return ret;
	return sysfs_emit(buf, "%02x\n", v);
}

static ssize_t data_store(struct kobject *kobj, struct kobj_attribute *attr,
			  const char *buf, size_t count)
{
	u8 v;
	int ret;

	if (kstrtou8(buf, 16, &v))
		return -EINVAL;
	ret = ec_write(ec_poke_addr, v);
	if (ret)
		return ret;
	return count;
}

static ssize_t dump_show(struct kobject *kobj, struct kobj_attribute *attr,
			 char *buf)
{
	int i, n = 0;
	u8 v;

	for (i = 0; i < 256; i++) {
		if ((i & 15) == 0)
			n += sysfs_emit_at(buf, n, "%02x:", i);
		if (ec_read(i, &v))
			n += sysfs_emit_at(buf, n, " --");
		else
			n += sysfs_emit_at(buf, n, " %02x", v);
		if ((i & 15) == 15)
			n += sysfs_emit_at(buf, n, "\n");
	}
	return n;
}

static struct kobj_attribute addr_attr = __ATTR(addr, 0600, addr_show, addr_store);
static struct kobj_attribute data_attr = __ATTR(data, 0600, data_show, data_store);
static struct kobj_attribute dump_attr = __ATTR(dump, 0400, dump_show, NULL);

static struct attribute *ec_poke_attrs[] = {
	&addr_attr.attr,
	&data_attr.attr,
	&dump_attr.attr,
	NULL,
};

static const struct attribute_group ec_poke_group = {
	.attrs = ec_poke_attrs,
};

static int __init ec_poke_init(void)
{
	int ret;
	u8 probe;

	/* ec_read() fails with -ENODEV when no EC has been registered */
	ret = ec_read(0, &probe);
	if (ret)
		return ret;
	ec_poke_kobj = kobject_create_and_add("ec_poke", kernel_kobj);
	if (!ec_poke_kobj)
		return -ENOMEM;
	ret = sysfs_create_group(ec_poke_kobj, &ec_poke_group);
	if (ret)
		kobject_put(ec_poke_kobj);
	return ret;
}

static void __exit ec_poke_exit(void)
{
	sysfs_remove_group(ec_poke_kobj, &ec_poke_group);
	kobject_put(ec_poke_kobj);
}

module_init(ec_poke_init);
module_exit(ec_poke_exit);

MODULE_DESCRIPTION("Byte-level access to the ACPI EC RAM through sysfs");
MODULE_LICENSE("GPL");
