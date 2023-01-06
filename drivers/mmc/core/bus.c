// SPDX-License-Identifier: GPL-2.0-only
/*
 *  linux/drivers/mmc/core/bus.c
 *
 *  Copyright (C) 2003 Russell King, All Rights Reserved.
 *  Copyright (C) 2007 Pierre Ossman
 *
 *  MMC card bus driver model
 */

#include <linux/export.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/stat.h>
#include <linux/of.h>
#include <linux/pm_runtime.h>
#include <linux/sysfs.h>

#include <linux/mmc/card.h>
#include <linux/mmc/host.h>

#include "core.h"
#include "card.h"
#include "host.h"
#include "sdio_cis.h"
#include "bus.h"

#define to_mmc_driver(d)	container_of(d, struct mmc_driver, drv)

/* DEVICE_ATTR用于在sysfs显示设备的属性
参考：https://www.jianshu.com/p/b923a5326e69
https://www.cnblogs.com/yikoulinux/p/15841343.html */

/* 属性的标准操作函数是show和store, 属性名叫type所以默认函数叫type_show */ 
/* sysfs_emit在sysfs的设备目录创建属性文件，用户态可以访问 */
static ssize_t type_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct mmc_card *card = mmc_dev_to_card(dev);

	switch (card->type) {
	case MMC_TYPE_MMC:
		return sysfs_emit(buf, "MMC\n");
	case MMC_TYPE_SD:
		return sysfs_emit(buf, "SD\n");
	case MMC_TYPE_SDIO:
		return sysfs_emit(buf, "SDIO\n");
	case MMC_TYPE_SD_COMBO:
		return sysfs_emit(buf, "SDcombo\n");
	default:
		return -EFAULT;
	}
}
static DEVICE_ATTR_RO(type); //创建属性名为type，默认方法是type_show
/* 定义mmc_dev属性，其中属性名为type */
static struct attribute *mmc_dev_attrs[] = {
	&dev_attr_type.attr, //这里格式是dev_attr_属性名，所以是dev_attr_type
	NULL,
};
ATTRIBUTE_GROUPS(mmc_dev); //注册mmc_dev

/* 对应用层udev、mdev提供MMC_TYPE，MMC_NAME等event类型的响应 */
static int
mmc_bus_uevent(struct device *dev, struct kobj_uevent_env *env)
{
	struct mmc_card *card = mmc_dev_to_card(dev);
	const char *type;
	unsigned int i;
	int retval = 0;

	switch (card->type) {
	case MMC_TYPE_MMC:
		type = "MMC";
		break;
	case MMC_TYPE_SD:
		type = "SD";
		break;
	case MMC_TYPE_SDIO:
		type = "SDIO";
		break;
	case MMC_TYPE_SD_COMBO:
		type = "SDcombo";
		break;
	default:
		type = NULL;
	}

	if (type) {
		retval = add_uevent_var(env, "MMC_TYPE=%s", type);
		if (retval)
			return retval;
	}

	if (mmc_card_sdio(card) || mmc_card_sd_combo(card)) {
		retval = add_uevent_var(env, "SDIO_ID=%04X:%04X",
					card->cis.vendor, card->cis.device);
		if (retval)
			return retval;

		retval = add_uevent_var(env, "SDIO_REVISION=%u.%u",
					card->major_rev, card->minor_rev);
		if (retval)
			return retval;

		for (i = 0; i < card->num_info; i++) {
			retval = add_uevent_var(env, "SDIO_INFO%u=%s", i+1, card->info[i]);
			if (retval)
				return retval;
		}
	}

	/*
	 * SDIO (non-combo) cards are not handled by mmc_block driver and do not
	 * have accessible CID register which used by mmc_card_name() function.
	 */
	if (mmc_card_sdio(card))
		return 0;

	retval = add_uevent_var(env, "MMC_NAME=%s", mmc_card_name(card));
	if (retval)
		return retval;

	/*
	 * Request the mmc_block device.  Note: that this is a direct request
	 * for the module it carries no information as to what is inserted.
	 */
	retval = add_uevent_var(env, "MODALIAS=mmc:block");

	return retval;
}

static int mmc_bus_probe(struct device *dev)
{
	struct mmc_driver *drv = to_mmc_driver(dev->driver);
	struct mmc_card *card = mmc_dev_to_card(dev);

	return drv->probe(card);
}

static void mmc_bus_remove(struct device *dev)
{
	struct mmc_driver *drv = to_mmc_driver(dev->driver);
	struct mmc_card *card = mmc_dev_to_card(dev);

	drv->remove(card);
}

static void mmc_bus_shutdown(struct device *dev)
{
	struct mmc_driver *drv = to_mmc_driver(dev->driver);
	struct mmc_card *card = mmc_dev_to_card(dev);
	struct mmc_host *host = card->host;
	int ret;

	if (dev->driver && drv->shutdown)
		drv->shutdown(card);

	if (host->bus_ops->shutdown) {
		ret = host->bus_ops->shutdown(host);
		if (ret)
			pr_warn("%s: error %d during shutdown\n",
				mmc_hostname(host), ret);
	}
}

#ifdef CONFIG_PM_SLEEP
static int mmc_bus_suspend(struct device *dev)
{
	struct mmc_card *card = mmc_dev_to_card(dev);
	struct mmc_host *host = card->host;
	int ret;

	ret = pm_generic_suspend(dev);
	if (ret)
		return ret;

	ret = host->bus_ops->suspend(host);
	if (ret)
		pm_generic_resume(dev);

	return ret;
}

static int mmc_bus_resume(struct device *dev)
{
	struct mmc_card *card = mmc_dev_to_card(dev);
	struct mmc_host *host = card->host;
	int ret;

	ret = host->bus_ops->resume(host);
	if (ret)
		pr_warn("%s: error %d during resume (card was removed?)\n",
			mmc_hostname(host), ret);

	ret = pm_generic_resume(dev);
	return ret;
}
#endif

#ifdef CONFIG_PM
static int mmc_runtime_suspend(struct device *dev)
{
	struct mmc_card *card = mmc_dev_to_card(dev);
	struct mmc_host *host = card->host;

	return host->bus_ops->runtime_suspend(host);
}

static int mmc_runtime_resume(struct device *dev)
{
	struct mmc_card *card = mmc_dev_to_card(dev);
	struct mmc_host *host = card->host;

	return host->bus_ops->runtime_resume(host);
}
#endif /* !CONFIG_PM */

static const struct dev_pm_ops mmc_bus_pm_ops = {
	SET_RUNTIME_PM_OPS(mmc_runtime_suspend, mmc_runtime_resume, NULL)
	SET_SYSTEM_SLEEP_PM_OPS(mmc_bus_suspend, mmc_bus_resume)
};

/* 定义mmc bus的属性和操作，bus的本质就是host对card操作的集合，因此可以理解bus主要属于card的范畴，只不过bus也关联host */
static struct bus_type mmc_bus_type = {
	/* bus是mmc */
	.name		= "mmc",
	/* mmc_dev_groups内部包含mmc_dev_attrs属性 */
	.dev_groups	= mmc_dev_groups,
	/* 没有.match接口是因为默认由mmc子系统实现的mmc driver匹配注册至mmc bus上的mmc card，不需在此定义.match */

	/* 添加该mmc bus的uevent参数:
	在调用device_add时，会调用kobject_uevent向应用层发送设备添加相关的事件,
	而kobject_uevent会调用该device所属bus和class的uevent接口，添加需要发送到应用的event参数 */
	.uevent		= mmc_bus_uevent,

	/* mmc card与mmc driver匹配成功后，会调用driver的probe进行探测初始化操作 */
	.probe		= mmc_bus_probe,
	/* mmc card与mmc driver解绑时，调用driver的remove进行注销操作 */
	.remove		= mmc_bus_remove,
	/* 调用driver和bus_ops的shutdown进行卡设备和bus掉电 */
	.shutdown	= mmc_bus_shutdown,
	/* mmc_bus本身的电源管理 */
	.pm		= &mmc_bus_pm_ops,
};

int mmc_register_bus(void)
{
	return bus_register(&mmc_bus_type);
}

void mmc_unregister_bus(void)
{
	bus_unregister(&mmc_bus_type);
}

/**
 *	mmc_register_driver - register a media driver
 *	@drv: MMC media driver
 */
int mmc_register_driver(struct mmc_driver *drv)
{
	drv->drv.bus = &mmc_bus_type;
	return driver_register(&drv->drv);
}

EXPORT_SYMBOL(mmc_register_driver);

/**
 *	mmc_unregister_driver - unregister a media driver
 *	@drv: MMC media driver
 */
void mmc_unregister_driver(struct mmc_driver *drv)
{
	drv->drv.bus = &mmc_bus_type;
	driver_unregister(&drv->drv);
}

EXPORT_SYMBOL(mmc_unregister_driver);

static void mmc_release_card(struct device *dev)
{
	struct mmc_card *card = mmc_dev_to_card(dev);

	sdio_free_common_cis(card);

	kfree(card->info);

	kfree(card);
}

/*
 * Allocate and initialise a new MMC card structure.
 */
/* 分配mmc_host相关的唯一卡数据结构mmc_card
流程和mmc_host分配流程类似，主要是卡结构的内存分配和卡设备注册 */
struct mmc_card *mmc_alloc_card(struct mmc_host *host, struct device_type *type)
{
	struct mmc_card *card;

	card = kzalloc(sizeof(struct mmc_card), GFP_KERNEL);
	if (!card)
		return ERR_PTR(-ENOMEM);

	card->host = host;

	device_initialize(&card->dev);

	card->dev.parent = mmc_classdev(host);
	/* 在此关联card和bus */
	card->dev.bus = &mmc_bus_type;
	card->dev.release = mmc_release_card;
	card->dev.type = type;

	return card;
}

/*
 * Register a new MMC card with the driver model.
 */
/* 卡设备注册到内核设备模型, 完成bus-driver-device模型中的bus-device关联
流程和mmc_host的设备注册流程类似，核心函数是device_add */
int mmc_add_card(struct mmc_card *card)
{
	int ret;
	const char *type;
	const char *speed_mode = "";
	const char *uhs_bus_speed_mode = "";
	static const char *const uhs_speeds[] = {
		[UHS_SDR12_BUS_SPEED] = "SDR12 ",
		[UHS_SDR25_BUS_SPEED] = "SDR25 ",
		[UHS_SDR50_BUS_SPEED] = "SDR50 ",
		[UHS_SDR104_BUS_SPEED] = "SDR104 ",
		[UHS_DDR50_BUS_SPEED] = "DDR50 ",
	};


	dev_set_name(&card->dev, "%s:%04x", mmc_hostname(card->host), card->rca);

	switch (card->type) {
	case MMC_TYPE_MMC:
		type = "MMC";
		break;
	case MMC_TYPE_SD:
		type = "SD";
		if (mmc_card_blockaddr(card)) {
			if (mmc_card_ext_capacity(card))
				type = "SDXC";
			else
				type = "SDHC";
		}
		break;
	case MMC_TYPE_SDIO:
		type = "SDIO";
		break;
	case MMC_TYPE_SD_COMBO:
		type = "SD-combo";
		if (mmc_card_blockaddr(card))
			type = "SDHC-combo";
		break;
	default:
		type = "?";
		break;
	}

	if (mmc_card_hs(card))
		speed_mode = "high speed ";
	else if (mmc_card_uhs(card))
		speed_mode = "UHS-I speed ";
	else if (mmc_card_uhs2(card->host))
		speed_mode = "UHS-II speed ";
	else if	(mmc_card_ddr52(card))
		speed_mode = "high speed DDR ";
	else if (mmc_card_hs200(card))
		speed_mode = "HS200 ";
	else if (mmc_card_hs400es(card))
		speed_mode = "HS400 Enhanced strobe ";
	else if (mmc_card_hs400(card))
		speed_mode = "HS400 ";

	if (mmc_card_uhs(card) &&
		(card->sd_bus_speed < ARRAY_SIZE(uhs_speeds)))
		uhs_bus_speed_mode = uhs_speeds[card->sd_bus_speed];

	if (mmc_host_is_spi(card->host))
		pr_info("%s: new %s%s card on SPI\n",
			mmc_hostname(card->host), speed_mode, type);
	else
		pr_info("%s: new %s%s%s card at address %04x\n",
			mmc_hostname(card->host), speed_mode,
			uhs_bus_speed_mode, type, card->rca);

#ifdef CONFIG_DEBUG_FS
	mmc_add_card_debugfs(card);
#endif
	card->dev.of_node = mmc_of_find_child_device(card->host, 0);

	device_enable_async_suspend(&card->dev);

	/* 老版本是使用device_register完成设备注册，
	device_register内部是device_initialize()和device_add()，这里使用device_add即可完成完成设备注册 
	参考：https://deepinout.com/linux-kernel-api/device-driver-and-device-management/linux-kernel-api-device_register.html
	*/
	ret = device_add(&card->dev);
	if (ret)
		return ret;

	mmc_card_set_present(card);

	return 0;
}

/*
 * Unregister a new MMC card with the driver model, and
 * (eventually) free it.
 */
/* 注销driver model中的卡设备，核心函数是device_del和put_device */
void mmc_remove_card(struct mmc_card *card)
{
	struct mmc_host *host = card->host;

#ifdef CONFIG_DEBUG_FS
	mmc_remove_card_debugfs(card);
#endif

	if (mmc_card_present(card)) {
		if (mmc_host_is_spi(card->host)) {
			pr_info("%s: SPI card removed\n",
				mmc_hostname(card->host));
		} else {
			pr_info("%s: card %04x removed\n",
				mmc_hostname(card->host), card->rca);
		}
		device_del(&card->dev);
		of_node_put(card->dev.of_node);
	}

	if (host->cqe_enabled) {
		host->cqe_ops->cqe_disable(host);
		host->cqe_enabled = false;
	}

	put_device(&card->dev);
}
