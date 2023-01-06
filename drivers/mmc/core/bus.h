/* SPDX-License-Identifier: GPL-2.0-only */
/*
 *  linux/drivers/mmc/core/bus.h
 *
 *  Copyright (C) 2003 Russell King, All Rights Reserved.
 *  Copyright 2007 Pierre Ossman
 */
#ifndef _MMC_CORE_BUS_H
#define _MMC_CORE_BUS_H

#include <linux/device.h>
#include <linux/sysfs.h>

struct mmc_host;
struct mmc_card;

#define MMC_DEV_ATTR(name, fmt, args...)					\
static ssize_t mmc_##name##_show (struct device *dev, struct device_attribute *attr, char *buf)	\
{										\
	struct mmc_card *card = mmc_dev_to_card(dev);				\
	return sysfs_emit(buf, fmt, args);					\
}										\
static DEVICE_ATTR(name, S_IRUGO, mmc_##name##_show, NULL)

struct mmc_card *mmc_alloc_card(struct mmc_host *host,
	struct device_type *type);
int mmc_add_card(struct mmc_card *card);
void mmc_remove_card(struct mmc_card *card);

int mmc_register_bus(void);
void mmc_unregister_bus(void);

/* 
mmc driver是通用抽象的mmc_card驱动，能做到通用是因为所有的mmc card的命令格式、状态流转、寄存器定义等操作都由Spec定义规则
mmc_driver的具体实例在block.c的mmc_blk_XXX实现 
*/
struct mmc_driver {
	struct device_driver drv;
	int (*probe)(struct mmc_card *card);
	void (*remove)(struct mmc_card *card);
	void (*shutdown)(struct mmc_card *card);
};

int mmc_register_driver(struct mmc_driver *drv);
void mmc_unregister_driver(struct mmc_driver *drv);

#endif
