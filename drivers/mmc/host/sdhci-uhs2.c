// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  linux/drivers/mmc/host/sdhci_uhs2.c - Secure Digital Host Controller
 *  Interface driver
 *
 *  Copyright (C) 2014 Intel Corp, All Rights Reserved.
 *  Copyright (C) 2020 Genesys Logic, Inc.
 *  Authors: Ben Chuang <ben.chuang@genesyslogic.com.tw>
 *  Copyright (C) 2020 Linaro Limited
 *  Author: AKASHI Takahiro <takahiro.akashi@linaro.org>
 */

#include <linux/delay.h>
#include <linux/dmaengine.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/mmc/mmc.h>
#include <linux/regulator/consumer.h>

#include "sdhci.h"
#include "sdhci-uhs2.h"

#define DRIVER_NAME "sdhci_uhs2"
#define DBG(f, x...) \
	pr_debug(DRIVER_NAME " [%s()]: " f, __func__, ## x)
#define SDHCI_UHS2_DUMP(f, x...) \
	pr_err("%s: " DRIVER_NAME ": " f, mmc_hostname(host->mmc), ## x)

void sdhci_uhs2_dump_regs(struct sdhci_host *host)
{
	if (!host->mmc || !(host->mmc->flags & MMC_UHS2_SUPPORT))
		return;

	SDHCI_UHS2_DUMP("==================== UHS2 ==================\n");
	SDHCI_UHS2_DUMP("Blk Size:  0x%08x | Blk Cnt:  0x%08x\n",
			sdhci_readw(host, SDHCI_UHS2_BLOCK_SIZE),
			sdhci_readl(host, SDHCI_UHS2_BLOCK_COUNT));
	SDHCI_UHS2_DUMP("Cmd:       0x%08x | Trn mode: 0x%08x\n",
			sdhci_readw(host, SDHCI_UHS2_COMMAND),
			sdhci_readw(host, SDHCI_UHS2_TRANS_MODE));
	SDHCI_UHS2_DUMP("Int Stat:  0x%08x | Dev Sel : 0x%08x\n",
			sdhci_readw(host, SDHCI_UHS2_DEV_INT_STATUS),
			sdhci_readb(host, SDHCI_UHS2_DEV_SELECT));
	SDHCI_UHS2_DUMP("Dev Int Code:  0x%08x\n",
			sdhci_readb(host, SDHCI_UHS2_DEV_INT_CODE));
	SDHCI_UHS2_DUMP("Reset:     0x%08x | Timer:    0x%08x\n",
			sdhci_readw(host, SDHCI_UHS2_SW_RESET),
			sdhci_readw(host, SDHCI_UHS2_TIMER_CTRL));
	SDHCI_UHS2_DUMP("ErrInt:    0x%08x | ErrIntEn: 0x%08x\n",
			sdhci_readl(host, SDHCI_UHS2_ERR_INT_STATUS),
			sdhci_readl(host, SDHCI_UHS2_ERR_INT_STATUS_EN));
	SDHCI_UHS2_DUMP("ErrSigEn:  0x%08x\n",
			sdhci_readl(host, SDHCI_UHS2_ERR_INT_SIG_EN));
}
EXPORT_SYMBOL_GPL(sdhci_uhs2_dump_regs);

/*****************************************************************************\
 *                                                                           *
 * Low level functions                                                       *
 *                                                                           *
\*****************************************************************************/

bool sdhci_uhs2_mode(struct sdhci_host *host)
{
	if ((host->mmc->caps2 & MMC_CAP2_SD_UHS2) &&
	    (IS_ENABLED(CONFIG_MMC_SDHCI_UHS2) &&
		(host->version >= SDHCI_SPEC_400) &&
		(host->mmc->flags & MMC_UHS2_SUPPORT)))
		return true;
	else
		return false;
}

/**
 * sdhci_uhs2_reset - invoke SW reset
 * @host: SDHCI host
 * @mask: Control mask
 *
 * Invoke SW reset, depending on a bit in @mask and wait for completion.
 */
void sdhci_uhs2_reset(struct sdhci_host *host, u16 mask)
{
	unsigned long timeout;
	u32 val;

	if (!(sdhci_uhs2_mode(host))) {
		/**
		 * u8  mask for legacy.
		 * u16 mask for uhs-2.
		 */
		u8 u8_mask;

		u8_mask = (mask & 0xFF);
		sdhci_reset(host, u8_mask);

		return;
	}

	sdhci_writew(host, mask, SDHCI_UHS2_SW_RESET);

	if (mask & SDHCI_UHS2_SW_RESET_FULL) {
		host->clock = 0;
		/* Reset-all turns off SD Bus Power */
		if (host->quirks2 & SDHCI_QUIRK2_CARD_ON_NEEDS_BUS_ON)
			sdhci_runtime_pm_bus_off(host);
	}

	/* Wait max 100 ms */
	timeout = 10000;

	/* hw clears the bit when it's done */
	if (read_poll_timeout_atomic(sdhci_readw, val, !(val & mask), 10,
				     timeout, true, host, SDHCI_UHS2_SW_RESET)) {
		pr_err("%s: %s: Reset 0x%x never completed.\n",
					       __func__, mmc_hostname(host->mmc), (int)mask);
		pr_err("%s: clean reset bit\n",
					       mmc_hostname(host->mmc));
		sdhci_writeb(host, 0, SDHCI_UHS2_SW_RESET);
		return;
	}
}
EXPORT_SYMBOL_GPL(sdhci_uhs2_reset);

void sdhci_uhs2_set_power(struct sdhci_host *host, unsigned char mode,
			  unsigned short vdd)
{
	struct mmc_host *mmc = host->mmc;
	u8 pwr;

	/* FIXME: check if flags & MMC_UHS2_SUPPORT? */
	if (!(sdhci_uhs2_mode(host))) {
		sdhci_set_power(host, mode, vdd);
		return;
	}

	if (mode != MMC_POWER_OFF) {
		pwr = sdhci_get_vdd_value(vdd);
		if (!pwr)
			WARN(1, "%s: Invalid vdd %#x\n",
			     mmc_hostname(host->mmc), vdd);
		pwr |= SDHCI_VDD2_POWER_180;
	}

	if (host->pwr == pwr)
		return;
	host->pwr = pwr;

	if (pwr == 0) {
		sdhci_writeb(host, 0, SDHCI_POWER_CONTROL);

		if (!IS_ERR(host->mmc->supply.vmmc))
			mmc_regulator_set_ocr(mmc, mmc->supply.vmmc, 0);
		if (!IS_ERR_OR_NULL(host->mmc->supply.vmmc2))
			mmc_regulator_set_ocr(mmc, mmc->supply.vmmc2, 0);

		if (host->quirks2 & SDHCI_QUIRK2_CARD_ON_NEEDS_BUS_ON)
			sdhci_runtime_pm_bus_off(host);
	} else {
		if (!IS_ERR(host->mmc->supply.vmmc))
			mmc_regulator_set_ocr(mmc, mmc->supply.vmmc, vdd);
		if (!IS_ERR_OR_NULL(host->mmc->supply.vmmc2))
			/* support 1.8v only for now */
			mmc_regulator_set_ocr(mmc, mmc->supply.vmmc2,
					      fls(MMC_VDD2_165_195) - 1);

		/*
		 * Spec says that we should clear the power reg before setting
		 * a new value. Some controllers don't seem to like this though.
		 */
		if (!(host->quirks & SDHCI_QUIRK_SINGLE_POWER_WRITE))
			sdhci_writeb(host, 0, SDHCI_POWER_CONTROL);

		/*
		 * At least the Marvell CaFe chip gets confused if we set the
		 * voltage and set turn on power at the same time, so set the
		 * voltage first.
		 */
		if (host->quirks & SDHCI_QUIRK_NO_SIMULT_VDD_AND_POWER)
			sdhci_writeb(host, pwr, SDHCI_POWER_CONTROL);

		/* vdd first */
		pwr |= SDHCI_POWER_ON;
		sdhci_writeb(host, pwr & 0xf, SDHCI_POWER_CONTROL);
		mdelay(5);

		pwr |= SDHCI_VDD2_POWER_ON;
		sdhci_writeb(host, pwr, SDHCI_POWER_CONTROL);
		mdelay(5);

		if (host->quirks2 & SDHCI_QUIRK2_CARD_ON_NEEDS_BUS_ON)
			sdhci_runtime_pm_bus_on(host);

		/*
		 * Some controllers need an extra 10ms delay of 10ms before
		 * they can apply clock after applying power
		 */
		if (host->quirks & SDHCI_QUIRK_DELAY_AFTER_POWER)
			mdelay(10);
	}
}
EXPORT_SYMBOL_GPL(sdhci_uhs2_set_power);

static u8 sdhci_calc_timeout_uhs2(struct sdhci_host *host, u8 *cmd_res,
				  u8 *dead_lock)
{
	u8 count;
	unsigned int cmd_res_timeout, dead_lock_timeout, current_timeout;

	/*
	 * If the host controller provides us with an incorrect timeout
	 * value, just skip the check and use 0xE.  The hardware may take
	 * longer to time out, but that's much better than having a too-short
	 * timeout value.
	 */
	if (host->quirks & SDHCI_QUIRK_BROKEN_TIMEOUT_VAL) {
		*cmd_res = 0xE;
		*dead_lock = 0xE;
		return 0xE;
	}

	/* timeout in us */
	cmd_res_timeout = 5 * 1000;
	dead_lock_timeout = 1 * 1000 * 1000;

	/*
	 * Figure out needed cycles.
	 * We do this in steps in order to fit inside a 32 bit int.
	 * The first step is the minimum timeout, which will have a
	 * minimum resolution of 6 bits:
	 * (1) 2^13*1000 > 2^22,
	 * (2) host->timeout_clk < 2^16
	 *     =>
	 *     (1) / (2) > 2^6
	 */
	count = 0;
	current_timeout = (1 << 13) * 1000 / host->timeout_clk;
	while (current_timeout < cmd_res_timeout) {
		count++;
		current_timeout <<= 1;
		if (count >= 0xF)
			break;
	}

	if (count >= 0xF) {
		DBG("%s: Too large timeout 0x%x requested for CMD_RES!\n",
		    mmc_hostname(host->mmc), count);
		count = 0xE;
	}
	*cmd_res = count;

	count = 0;
	current_timeout = (1 << 13) * 1000 / host->timeout_clk;
	while (current_timeout < dead_lock_timeout) {
		count++;
		current_timeout <<= 1;
		if (count >= 0xF)
			break;
	}

	if (count >= 0xF) {
		DBG("%s: Too large timeout 0x%x requested for DEADLOCK!\n",
		    mmc_hostname(host->mmc), count);
		count = 0xE;
	}
	*dead_lock = count;

	return count;
}

static void __sdhci_uhs2_set_timeout(struct sdhci_host *host)
{
	u8 cmd_res, dead_lock;

	sdhci_calc_timeout_uhs2(host, &cmd_res, &dead_lock);
	cmd_res |= dead_lock << SDHCI_UHS2_TIMER_CTRL_DEADLOCK_SHIFT;
	sdhci_writeb(host, cmd_res, SDHCI_UHS2_TIMER_CTRL);
}

void sdhci_uhs2_set_timeout(struct sdhci_host *host, struct mmc_command *cmd)
{
	__sdhci_set_timeout(host, cmd);

	if (host->mmc->flags & MMC_UHS2_SUPPORT)
		__sdhci_uhs2_set_timeout(host);
}
EXPORT_SYMBOL_GPL(sdhci_uhs2_set_timeout);

/**
 * sdhci_uhs2_clear_set_irqs - set Error Interrupt Status Enable register
 * @host:	SDHCI host
 * @clear:	bit-wise clear mask
 * @set:	bit-wise set mask
 *
 * Set/unset bits in UHS-II Error Interrupt Status Enable register
 */
void sdhci_uhs2_clear_set_irqs(struct sdhci_host *host, u32 clear, u32 set)
{
	u32 ier;

	ier = sdhci_readl(host, SDHCI_UHS2_ERR_INT_STATUS_EN);
	ier &= ~clear;
	ier |= set;
	sdhci_writel(host, ier, SDHCI_UHS2_ERR_INT_STATUS_EN);
	sdhci_writel(host, ier, SDHCI_UHS2_ERR_INT_SIG_EN);
}
EXPORT_SYMBOL_GPL(sdhci_uhs2_clear_set_irqs);

static void __sdhci_uhs2_set_ios(struct mmc_host *mmc, struct mmc_ios *ios)
{
	struct sdhci_host *host = mmc_priv(mmc);
	u8 cmd_res, dead_lock;
	u16 ctrl_2;
	unsigned long flags;

	/* FIXME: why lock? */
	spin_lock_irqsave(&host->lock, flags);

	/* UHS2 Timeout Control */
	sdhci_calc_timeout_uhs2(host, &cmd_res, &dead_lock);

	/* change to use calculate value */
	cmd_res |= dead_lock << SDHCI_UHS2_TIMER_CTRL_DEADLOCK_SHIFT;

	sdhci_uhs2_clear_set_irqs(host,
				  SDHCI_UHS2_ERR_INT_STATUS_RES_TIMEOUT |
				  SDHCI_UHS2_ERR_INT_STATUS_DEADLOCK_TIMEOUT,
				  0);
	sdhci_writeb(host, cmd_res, SDHCI_UHS2_TIMER_CTRL);
	sdhci_uhs2_clear_set_irqs(host, 0,
				  SDHCI_UHS2_ERR_INT_STATUS_RES_TIMEOUT |
				  SDHCI_UHS2_ERR_INT_STATUS_DEADLOCK_TIMEOUT);

	/* UHS2 timing */
	ctrl_2 = sdhci_readw(host, SDHCI_HOST_CONTROL2);
	if (ios->timing == MMC_TIMING_SD_UHS2)
		ctrl_2 |= SDHCI_CTRL_UHS_2 | SDHCI_CTRL_UHS2_INTERFACE_EN;
	else
		ctrl_2 &= ~(SDHCI_CTRL_UHS_2 | SDHCI_CTRL_UHS2_INTERFACE_EN);
	sdhci_writew(host, ctrl_2, SDHCI_HOST_CONTROL2);

	if (!(host->quirks2 & SDHCI_QUIRK2_PRESET_VALUE_BROKEN))
		sdhci_enable_preset_value(host, true);

	if (host->ops->set_power)
		host->ops->set_power(host, ios->power_mode, ios->vdd);
	else
		sdhci_uhs2_set_power(host, ios->power_mode, ios->vdd);
	udelay(100);

	host->timing = ios->timing;
	sdhci_set_clock(host, host->clock);

	spin_unlock_irqrestore(&host->lock, flags);
}

static void sdhci_uhs2_set_config(struct sdhci_host *host)
{
	u32 value;
	u16 sdhci_uhs2_set_ptr = sdhci_readw(host, SDHCI_UHS2_SET_PTR);
	u16 sdhci_uhs2_gen_set_reg = (sdhci_uhs2_set_ptr + 0);
	u16 sdhci_uhs2_phy_set_reg = (sdhci_uhs2_set_ptr + 4);
	u16 sdhci_uhs2_tran_set_reg = (sdhci_uhs2_set_ptr + 8);
	u16 sdhci_uhs2_tran_set_1_reg = (sdhci_uhs2_set_ptr + 12);

	/* Set Gen Settings */
	sdhci_writel(host, host->mmc->uhs2_caps.n_lanes_set <<
		SDHCI_UHS2_GEN_SET_N_LANES_POS, sdhci_uhs2_gen_set_reg);

	/* Set PHY Settings */
	value = (host->mmc->uhs2_caps.n_lss_dir_set <<
			SDHCI_UHS2_PHY_SET_N_LSS_DIR_POS) |
		(host->mmc->uhs2_caps.n_lss_sync_set <<
			SDHCI_UHS2_PHY_SET_N_LSS_SYN_POS);
	if (host->mmc->flags & MMC_UHS2_SPEED_B)
		value |= 1 << SDHCI_UHS2_PHY_SET_SPEED_POS;
	sdhci_writel(host, value, sdhci_uhs2_phy_set_reg);

	/* Set LINK-TRAN Settings */
	value = (host->mmc->uhs2_caps.max_retry_set <<
			SDHCI_UHS2_TRAN_SET_RETRY_CNT_POS) |
		(host->mmc->uhs2_caps.n_fcu_set <<
			SDHCI_UHS2_TRAN_SET_N_FCU_POS);
	sdhci_writel(host, value, sdhci_uhs2_tran_set_reg);
	sdhci_writel(host, host->mmc->uhs2_caps.n_data_gap_set,
		     sdhci_uhs2_tran_set_1_reg);
}

static int sdhci_uhs2_check_dormant(struct sdhci_host *host)
{
	u32 val;
	/* 100ms */
	int timeout = 100000;

	if (read_poll_timeout_atomic(sdhci_readl, val, (val & SDHCI_UHS2_IN_DORMANT_STATE),
				     100, timeout, true, host, SDHCI_PRESENT_STATE)) {
		pr_warn("%s: UHS2 IN_DORMANT fail in 100ms.\n", mmc_hostname(host->mmc));
		sdhci_dumpregs(host);
		return -EIO;
	}
	return 0;
}

/*****************************************************************************\
 *                                                                           *
 * MMC callbacks                                                             *
 *                                                                           *
\*****************************************************************************/

static int sdhci_uhs2_start_signal_voltage_switch(struct mmc_host *mmc,
						  struct mmc_ios *ios)
{
	struct sdhci_host *host = mmc_priv(mmc);

	/*
	 * For UHS2, the signal voltage is supplied by vdd2 which is
	 * already 1.8v so no voltage switch required.
	 */
	if (sdhci_uhs2_mode(host))
		return 0;

	return sdhci_start_signal_voltage_switch(mmc, ios);
}

int sdhci_uhs2_set_ios(struct mmc_host *mmc, struct mmc_ios *ios)
{
	struct sdhci_host *host = mmc_priv(mmc);

	if (!(host->version >= SDHCI_SPEC_400) ||
	    !(host->mmc->flags & MMC_UHS2_SUPPORT &&
	      host->mmc->caps2 & MMC_CAP2_SD_UHS2)) {
		sdhci_set_ios(mmc, ios);
		return 0;
	}

	if (ios->power_mode == MMC_POWER_UNDEFINED)
		return 1;

	if (host->flags & SDHCI_DEVICE_DEAD) {
		if (!IS_ERR(mmc->supply.vmmc) &&
		    ios->power_mode == MMC_POWER_OFF)
			mmc_regulator_set_ocr(mmc, mmc->supply.vmmc, 0);
		if (!IS_ERR_OR_NULL(mmc->supply.vmmc2) &&
		    ios->power_mode == MMC_POWER_OFF)
			mmc_regulator_set_ocr(mmc, mmc->supply.vmmc2, 0);
		return 1;
	}

	/* FIXME: host->timing = ios->timing */

	sdhci_set_ios_common(mmc, ios);

	__sdhci_uhs2_set_ios(mmc, ios);

	return 0;
}

static int sdhci_uhs2_disable_clk(struct mmc_host *mmc)
{
	struct sdhci_host *host = mmc_priv(mmc);
	u16 clk = sdhci_readw(host, SDHCI_CLOCK_CONTROL);

	clk &= ~SDHCI_CLOCK_CARD_EN;
	sdhci_writew(host, clk, SDHCI_CLOCK_CONTROL);

	return 0;
}

static int sdhci_uhs2_enable_clk(struct mmc_host *mmc)
{
	struct sdhci_host *host = mmc_priv(mmc);
	u16 clk = sdhci_readw(host, SDHCI_CLOCK_CONTROL);
	u32 val;
	/* 20ms */
	int timeout_us = 20000;

	clk |= SDHCI_CLOCK_CARD_EN;
	sdhci_writew(host, clk, SDHCI_CLOCK_CONTROL);

	if (read_poll_timeout_atomic(sdhci_readw, val, (val & SDHCI_CLOCK_INT_STABLE),
				     10, timeout_us, true, host, SDHCI_CLOCK_CONTROL)) {
		pr_err("%s: Internal clock never stabilised.\n", mmc_hostname(host->mmc));
		sdhci_dumpregs(host);
		return 1;
	}
	return 0;
}

static int sdhci_uhs2_do_detect_init(struct mmc_host *mmc);

static int sdhci_uhs2_control(struct mmc_host *mmc, enum sd_uhs2_operation op)
{
	struct sdhci_host *host = mmc_priv(mmc);
	unsigned long flags;
	int err = 0;
	u16 sdhci_uhs2_set_ptr = sdhci_readw(host, SDHCI_UHS2_SET_PTR);
	u16 sdhci_uhs2_phy_set_reg = (sdhci_uhs2_set_ptr + 4);

	DBG("Begin %s, act %d.\n", __func__, op);

	spin_lock_irqsave(&host->lock, flags);

	switch (op) {
	case UHS2_PHY_INIT:
		err = sdhci_uhs2_do_detect_init(mmc);
		break;
	case UHS2_SET_CONFIG:
		sdhci_uhs2_set_config(host);
		break;
	case UHS2_ENABLE_INT:
		sdhci_clear_set_irqs(host, 0, SDHCI_INT_CARD_INT);
		break;
	case UHS2_DISABLE_INT:
		sdhci_clear_set_irqs(host, SDHCI_INT_CARD_INT, 0);
		break;
	case UHS2_SET_SPEED_B:
		sdhci_writeb(host, 1 << SDHCI_UHS2_PHY_SET_SPEED_POS,
			     sdhci_uhs2_phy_set_reg);
		break;
	case UHS2_CHECK_DORMANT:
		err = sdhci_uhs2_check_dormant(host);
		break;
	case UHS2_DISABLE_CLK:
		err = sdhci_uhs2_disable_clk(mmc);
		break;
	case UHS2_ENABLE_CLK:
		err = sdhci_uhs2_enable_clk(mmc);
		break;
	case UHS2_POST_ATTACH_SD:
		host->ops->uhs2_post_attach_sd(host);
		break;
	default:
		pr_err("%s: input sd uhs2 operation %d is wrong!\n",
		       mmc_hostname(host->mmc), op);
		err = -EIO;
		break;
	}

	spin_unlock_irqrestore(&host->lock, flags);

	return err;
}

/*****************************************************************************\
 *                                                                           *
 * Core functions                                                            *
 *                                                                           *
\*****************************************************************************/

static void sdhci_uhs2_prepare_data(struct sdhci_host *host,
				    struct mmc_command *cmd)
{
	struct mmc_data *data = cmd->data;

	sdhci_initialize_data(host, data);

	sdhci_prepare_dma(host, data);

	sdhci_writew(host, data->blksz, SDHCI_UHS2_BLOCK_SIZE);
	sdhci_writew(host, data->blocks, SDHCI_UHS2_BLOCK_COUNT);
}

#if IS_ENABLED(CONFIG_MMC_SDHCI_EXTERNAL_DMA)
static void sdhci_uhs2_external_dma_prepare_data(struct sdhci_host *host,
						 struct mmc_command *cmd)
{
	if (!sdhci_external_dma_setup(host, cmd)) {
		__sdhci_external_dma_prepare_data(host, cmd);
	} else {
		sdhci_external_dma_release(host);
		pr_err("%s: Cannot use external DMA, switch to the DMA/PIO which standard SDHCI provides.\n",
		       mmc_hostname(host->mmc));
		sdhci_uhs2_prepare_data(host, cmd);
	}
}
#else
static inline void sdhci_uhs2_external_dma_prepare_data(struct sdhci_host *host,
							struct mmc_command *cmd)
{
	/* This should never happen */
	WARN_ON_ONCE(1);
}

static inline void sdhci_external_dma_pre_transfer(struct sdhci_host *host,
						   struct mmc_command *cmd)
{
}

static inline struct dma_chan *sdhci_external_dma_channel(struct sdhci_host *host,
							  struct mmc_data *data)
{
	return NULL;
}
#endif /* CONFIG_MMC_SDHCI_EXTERNAL_DMA */

static void sdhci_uhs2_finish_data(struct sdhci_host *host)
{
	struct mmc_data *data = host->data;

	__sdhci_finish_data_common(host);

	/*
	 *  FIXME: Is this condition needed?
	    if (host->mmc->flags & MMC_UHS2_INITIALIZED)
	 */
	__sdhci_finish_mrq(host, data->mrq);
}

static void sdhci_uhs2_set_transfer_mode(struct sdhci_host *host,
					 struct mmc_command *cmd)
{
	u16 mode;
	struct mmc_data *data = cmd->data;
	u16 arg;

	if (!data) {
		/* clear Auto CMD settings for no data CMDs */
		arg = cmd->uhs2_cmd->arg;
		if ((((arg & 0xF) << 8) | ((arg >> 8) & 0xFF)) ==
		       UHS2_DEV_CMD_TRANS_ABORT) {
			mode =  0;
		} else {
			mode = sdhci_readw(host, SDHCI_UHS2_TRANS_MODE);
			if (cmd->opcode == MMC_STOP_TRANSMISSION ||
			    cmd->opcode == MMC_ERASE)
				mode |= SDHCI_UHS2_TRNS_WAIT_EBSY;
			else
				/* send status mode */
				if (cmd->opcode == MMC_SEND_STATUS)
					mode = 0;
		}

		if (IS_ENABLED(CONFIG_MMC_DEBUG))
			DBG("UHS2 no data trans mode is 0x%x.\n", mode);

		sdhci_writew(host, mode, SDHCI_UHS2_TRANS_MODE);
		return;
	}

	WARN_ON(!host->data);

	mode = SDHCI_UHS2_TRNS_BLK_CNT_EN | SDHCI_UHS2_TRNS_WAIT_EBSY;
	if (data->flags & MMC_DATA_WRITE)
		mode |= SDHCI_UHS2_TRNS_DATA_TRNS_WRT;

	if (data->blocks == 1 &&
	    data->blksz != 512 &&
	    cmd->opcode != MMC_READ_SINGLE_BLOCK &&
	    cmd->opcode != MMC_WRITE_BLOCK) {
		mode &= ~SDHCI_UHS2_TRNS_BLK_CNT_EN;
		mode |= SDHCI_UHS2_TRNS_BLK_BYTE_MODE;
	}

	if (host->flags & SDHCI_REQ_USE_DMA)
		mode |= SDHCI_UHS2_TRNS_DMA;

	if ((host->mmc->uhs2_ios.is_2L_HD_mode) && !cmd->uhs2_tmode0_flag)
		mode |= SDHCI_UHS2_TRNS_2L_HD;

	sdhci_writew(host, mode, SDHCI_UHS2_TRANS_MODE);

	if (IS_ENABLED(CONFIG_MMC_DEBUG))
		DBG("UHS2 trans mode is 0x%x.\n", mode);
}

static void __sdhci_uhs2_send_command(struct sdhci_host *host,
				      struct mmc_command *cmd)
{
	int i, j;
	int cmd_reg;

	if (host->mmc->flags & MMC_UHS2_INITIALIZED) {
		if (!cmd->uhs2_cmd) {
			pr_err("%s: fatal error, no uhs2_cmd!\n",
			       mmc_hostname(host->mmc));
			return;
		}
	}

	i = 0;
	sdhci_writel(host,
		     ((u32)cmd->uhs2_cmd->arg << 16) |
				(u32)cmd->uhs2_cmd->header,
		     SDHCI_UHS2_CMD_PACKET + i);
	i += 4;

	/*
	 * Per spec, playload (config) should be MSB before sending out.
	 * But we don't need convert here because had set payload as
	 * MSB when preparing config read/write commands.
	 */
	for (j = 0; j < cmd->uhs2_cmd->payload_len / sizeof(u32); j++) {
		sdhci_writel(host, *(cmd->uhs2_cmd->payload + j),
			     SDHCI_UHS2_CMD_PACKET + i);
		i += 4;
	}

	for ( ; i < SDHCI_UHS2_CMD_PACK_MAX_LEN; i += 4)
		sdhci_writel(host, 0, SDHCI_UHS2_CMD_PACKET + i);

	if (IS_ENABLED(CONFIG_MMC_DEBUG)) {
		DBG("UHS2 CMD packet_len = %d.\n", cmd->uhs2_cmd->packet_len);
		for (i = 0; i < cmd->uhs2_cmd->packet_len; i++)
			DBG("UHS2 CMD_PACKET[%d] = 0x%x.\n", i,
			    sdhci_readb(host, SDHCI_UHS2_CMD_PACKET + i));
	}

	cmd_reg = cmd->uhs2_cmd->packet_len <<
		SDHCI_UHS2_COMMAND_PACK_LEN_SHIFT;
	if ((cmd->flags & MMC_CMD_MASK) == MMC_CMD_ADTC)
		cmd_reg |= SDHCI_UHS2_COMMAND_DATA;
	if (cmd->opcode == MMC_STOP_TRANSMISSION)
		cmd_reg |= SDHCI_UHS2_COMMAND_CMD12;

	/* UHS2 Native ABORT */
	if ((cmd->uhs2_cmd->header & UHS2_NATIVE_PACKET) &&
	    ((((cmd->uhs2_cmd->arg & 0xF) << 8) |
	    ((cmd->uhs2_cmd->arg >> 8) & 0xFF)) == UHS2_DEV_CMD_TRANS_ABORT))
		cmd_reg |= SDHCI_UHS2_COMMAND_TRNS_ABORT;

	/* UHS2 Native DORMANT */
	if ((cmd->uhs2_cmd->header & UHS2_NATIVE_PACKET) &&
	    ((((cmd->uhs2_cmd->arg & 0xF) << 8) |
	     ((cmd->uhs2_cmd->arg >> 8) & 0xFF)) ==
				UHS2_DEV_CMD_GO_DORMANT_STATE))
		cmd_reg |= SDHCI_UHS2_COMMAND_DORMANT;

	DBG("0x%x is set to UHS2 CMD register.\n", cmd_reg);

	sdhci_writew(host, cmd_reg, SDHCI_UHS2_COMMAND);
}

static bool sdhci_uhs2_send_command(struct sdhci_host *host,
				    struct mmc_command *cmd)
{
	int flags;
	u32 mask;
	unsigned long timeout;

	WARN_ON(host->cmd);

	/* Initially, a command has no error */
	cmd->error = 0;

	if (!(host->mmc->flags & MMC_UHS2_SUPPORT))
		return sdhci_send_command(host, cmd);

	if (cmd->opcode == MMC_STOP_TRANSMISSION)
		cmd->flags |= MMC_RSP_BUSY;

	mask = SDHCI_CMD_INHIBIT;

	if (sdhci_readl(host, SDHCI_PRESENT_STATE) & mask)
		return false;

	host->cmd = cmd;
	host->data_timeout = 0;
	if (sdhci_data_line_cmd(cmd)) {
		WARN_ON(host->data_cmd);
		host->data_cmd = cmd;
		__sdhci_uhs2_set_timeout(host);
	}

	if (cmd->data) {
		if (host->use_external_dma)
			sdhci_uhs2_external_dma_prepare_data(host, cmd);
		else
			sdhci_uhs2_prepare_data(host, cmd);
	}

	sdhci_uhs2_set_transfer_mode(host, cmd);

	if ((cmd->flags & MMC_RSP_136) && (cmd->flags & MMC_RSP_BUSY)) {
		WARN_ONCE(1, "Unsupported response type!\n");
		/*
		 * This does not happen in practice because 136-bit response
		 * commands never have busy waiting, so rather than complicate
		 * the error path, just remove busy waiting and continue.
		 */
		cmd->flags &= ~MMC_RSP_BUSY;
	}

	if (!(cmd->flags & MMC_RSP_PRESENT))
		flags = SDHCI_CMD_RESP_NONE;
	else if (cmd->flags & MMC_RSP_136)
		flags = SDHCI_CMD_RESP_LONG;
	else if (cmd->flags & MMC_RSP_BUSY)
		flags = SDHCI_CMD_RESP_SHORT_BUSY;
	else
		flags = SDHCI_CMD_RESP_SHORT;

	if (cmd->flags & MMC_RSP_CRC)
		flags |= SDHCI_CMD_CRC;
	if (cmd->flags & MMC_RSP_OPCODE)
		flags |= SDHCI_CMD_INDEX;

	timeout = jiffies;
	if (host->data_timeout)
		timeout += nsecs_to_jiffies(host->data_timeout);
	else if (!cmd->data && cmd->busy_timeout > 9000)
		timeout += DIV_ROUND_UP(cmd->busy_timeout, 1000) * HZ + HZ;
	else
		timeout += 10 * HZ;
	sdhci_mod_timer(host, cmd->mrq, timeout);

	if (host->use_external_dma)
		sdhci_external_dma_pre_transfer(host, cmd);

	__sdhci_uhs2_send_command(host, cmd);

	return true;
}

static bool sdhci_uhs2_send_command_retry(struct sdhci_host *host,
				     struct mmc_command *cmd,
				     unsigned long flags)
	__releases(host->lock)
	__acquires(host->lock)
{
	struct mmc_command *deferred_cmd = host->deferred_cmd;
	int timeout = 10; /* Approx. 10 ms */
	bool present;

	while (!sdhci_uhs2_send_command(host, cmd)) {
		if (!timeout--) {
			pr_err("%s: Controller never released inhibit bit(s).\n",
			       mmc_hostname(host->mmc));
			sdhci_dumpregs(host);
			cmd->error = -EIO;
			return false;
		}

		spin_unlock_irqrestore(&host->lock, flags);

		usleep_range(1000, 1250);

		present = host->mmc->ops->get_cd(host->mmc);

		spin_lock_irqsave(&host->lock, flags);

		/* A deferred command might disappear, handle that */
		if (cmd == deferred_cmd && cmd != host->deferred_cmd)
			return true;

		if (sdhci_present_error(host, cmd, present))
			return false;
	}

	if (cmd == host->deferred_cmd)
		host->deferred_cmd = NULL;

	return true;
}

static void __sdhci_uhs2_finish_command(struct sdhci_host *host)
{
	struct mmc_command *cmd = host->cmd;
	u8 resp;
	u8 ecode;
	bool bReadA0 = 0;
	int i;

	if (host->mmc->flags & MMC_UHS2_INITIALIZED) {
		resp = sdhci_readb(host, SDHCI_UHS2_RESPONSE + 2);
		if (resp & UHS2_RES_NACK_MASK) {
			ecode = (resp >> UHS2_RES_ECODE_POS) &
				UHS2_RES_ECODE_MASK;
			pr_err("%s: NACK is got, ECODE=0x%x.\n",
			       mmc_hostname(host->mmc), ecode);
		}
		bReadA0 = 1;
	}

	if (cmd->uhs2_resp &&
	    cmd->uhs2_resp_len && cmd->uhs2_resp_len <= 20) {
		/* Get whole response of some native CCMD, like
		 * DEVICE_INIT, ENUMERATE.
		 */
		for (i = 0; i < cmd->uhs2_resp_len; i++)
			cmd->uhs2_resp[i] =
				sdhci_readb(host, SDHCI_UHS2_RESPONSE + i);
	} else {
		/* Get SD CMD response and Payload for some read
		 * CCMD, like INQUIRY_CFG.
		 */
		/* Per spec (p136), payload field is divided into
		 * a unit of DWORD and transmission order within
		 * a DWORD is big endian.
		 */
		if (!bReadA0)
			sdhci_readl(host, SDHCI_UHS2_RESPONSE);
		for (i = 4; i < 20; i += 4) {
			cmd->resp[i / 4 - 1] =
				(sdhci_readb(host,
					     SDHCI_UHS2_RESPONSE + i) << 24) |
				(sdhci_readb(host,
					     SDHCI_UHS2_RESPONSE + i + 1)
					<< 16) |
				(sdhci_readb(host,
					     SDHCI_UHS2_RESPONSE + i + 2)
					<< 8) |
				sdhci_readb(host, SDHCI_UHS2_RESPONSE + i + 3);
		}
	}
}

static void sdhci_uhs2_finish_command(struct sdhci_host *host)
{
	struct mmc_command *cmd = host->cmd;

	/* FIXME: Is this check necessary? */
	if (!(host->mmc->flags & MMC_UHS2_SUPPORT)) {
		sdhci_finish_command(host);
		return;
	}

	__sdhci_uhs2_finish_command(host);

	host->cmd = NULL;

	if (cmd->mrq->cap_cmd_during_tfr && cmd == cmd->mrq->cmd)
		mmc_command_done(host->mmc, cmd->mrq);

	/*
	 * The host can send and interrupt when the busy state has
	 * ended, allowing us to wait without wasting CPU cycles.
	 * The busy signal uses DAT0 so this is similar to waiting
	 * for data to complete.
	 *
	 * Note: The 1.0 specification is a bit ambiguous about this
	 *       feature so there might be some problems with older
	 *       controllers.
	 */
	if (cmd->flags & MMC_RSP_BUSY) {
		if (cmd->data) {
			DBG("Cannot wait for busy signal when also doing a data transfer");
		} else if (!(host->quirks & SDHCI_QUIRK_NO_BUSY_IRQ) &&
			   cmd == host->data_cmd) {
			/* Command complete before busy is ended */
			return;
		}
	}

	/* Processed actual command. */
	if (host->data && host->data_early)
		sdhci_uhs2_finish_data(host);

	if (!cmd->data)
		__sdhci_finish_mrq(host, cmd->mrq);
}

/*****************************************************************************\
 *                                                                           *
 * Request done                                                              *
 *                                                                           *
\*****************************************************************************/

static bool sdhci_uhs2_request_done(struct sdhci_host *host)
{
	unsigned long flags;
	struct mmc_request *mrq;
	int i;

	/* FIXME: UHS2_INITIALIZED, instead? */
	if (!(host->mmc->flags & MMC_UHS2_SUPPORT))
		return sdhci_request_done(host);

	spin_lock_irqsave(&host->lock, flags);

	for (i = 0; i < SDHCI_MAX_MRQS; i++) {
		mrq = host->mrqs_done[i];
		if (mrq)
			break;
	}

	if (!mrq) {
		spin_unlock_irqrestore(&host->lock, flags);
		return true;
	}

	/*
	 * Always unmap the data buffers if they were mapped by
	 * sdhci_prepare_data() whenever we finish with a request.
	 * This avoids leaking DMA mappings on error.
	 */
	if (host->flags & SDHCI_REQ_USE_DMA) {
		struct mmc_data *data = mrq->data;

		if (host->use_external_dma && data &&
		    (mrq->cmd->error || data->error)) {
			struct dma_chan *chan = sdhci_external_dma_channel(host, data);

			host->mrqs_done[i] = NULL;
			spin_unlock_irqrestore(&host->lock, flags);
			dmaengine_terminate_sync(chan);
			spin_lock_irqsave(&host->lock, flags);
			sdhci_set_mrq_done(host, mrq);
		}

		sdhci_request_done_dma(host, mrq);
	}

	/*
	 * The controller needs a reset of internal state machines
	 * upon error conditions.
	 */
	if (sdhci_needs_reset(host, mrq)) {
		/*
		 * Do not finish until command and data lines are available for
		 * reset. Note there can only be one other mrq, so it cannot
		 * also be in mrqs_done, otherwise host->cmd and host->data_cmd
		 * would both be null.
		 */
		if (host->cmd || host->data_cmd) {
			spin_unlock_irqrestore(&host->lock, flags);
			return true;
		}

		/* Some controllers need this kick or reset won't work here */
		if (host->quirks & SDHCI_QUIRK_CLOCK_BEFORE_RESET)
			/* This is to force an update */
			host->ops->set_clock(host, host->clock);

		host->ops->uhs2_reset(host, SDHCI_UHS2_SW_RESET_SD);
		host->pending_reset = false;
	}

	host->mrqs_done[i] = NULL;

	spin_unlock_irqrestore(&host->lock, flags);

	if (host->ops->request_done)
		host->ops->request_done(host, mrq);
	else
		mmc_request_done(host->mmc, mrq);

	return false;
}

static void sdhci_uhs2_complete_work(struct work_struct *work)
{
	struct sdhci_host *host = container_of(work, struct sdhci_host,
					       complete_work);

	while (!sdhci_uhs2_request_done(host))
		;
}

/*****************************************************************************\
 *                                                                           *
 * Interrupt handling                                                        *
 *                                                                           *
\*****************************************************************************/

static void __sdhci_uhs2_irq(struct sdhci_host *host, u32 uhs2mask)
{
	struct mmc_command *cmd = host->cmd;

	DBG("*** %s got UHS2 error interrupt: 0x%08x\n",
	    mmc_hostname(host->mmc), uhs2mask);

	if (uhs2mask & SDHCI_UHS2_ERR_INT_STATUS_CMD_MASK) {
		if (!host->cmd) {
			pr_err("%s: Got cmd interrupt 0x%08x but no cmd.\n",
			       mmc_hostname(host->mmc),
			       (unsigned int)uhs2mask);
			sdhci_dumpregs(host);
			return;
		}
		host->cmd->error = -EILSEQ;
		if (uhs2mask & SDHCI_UHS2_ERR_INT_STATUS_RES_TIMEOUT)
			host->cmd->error = -ETIMEDOUT;
	}

	if (uhs2mask & SDHCI_UHS2_ERR_INT_STATUS_DATA_MASK) {
		if (!host->data) {
			pr_err("%s: Got data interrupt 0x%08x but no data.\n",
			       mmc_hostname(host->mmc),
			       (unsigned int)uhs2mask);
			sdhci_dumpregs(host);
			return;
		}

		if (uhs2mask & SDHCI_UHS2_ERR_INT_STATUS_DEADLOCK_TIMEOUT) {
			pr_err("%s: Got deadlock timeout interrupt 0x%08x\n",
			       mmc_hostname(host->mmc),
			       (unsigned int)uhs2mask);
			host->data->error = -ETIMEDOUT;
		} else if (uhs2mask & SDHCI_UHS2_ERR_INT_STATUS_ADMA) {
			pr_err("%s: ADMA error = 0x %x\n",
			       mmc_hostname(host->mmc),
			       sdhci_readb(host, SDHCI_ADMA_ERROR));
			host->data->error = -EIO;
		} else {
			host->data->error = -EILSEQ;
		}
	}

	if (host->data && host->data->error)
		sdhci_uhs2_finish_data(host);
	else
		sdhci_finish_mrq(host, cmd->mrq);
}

u32 sdhci_uhs2_irq(struct sdhci_host *host, u32 intmask)
{
	u32 mask = intmask, uhs2mask;

	if (!(host->mmc->flags & MMC_UHS2_SUPPORT))
		goto out;

	if (intmask & SDHCI_INT_ERROR) {
		uhs2mask = sdhci_readl(host, SDHCI_UHS2_ERR_INT_STATUS);
		if (!(uhs2mask & SDHCI_UHS2_ERR_INT_STATUS_MASK))
			goto cmd_irq;

		/* Clear error interrupts */
		sdhci_writel(host, uhs2mask & SDHCI_UHS2_ERR_INT_STATUS_MASK,
			     SDHCI_UHS2_ERR_INT_STATUS);

		/* Handle error interrupts */
		__sdhci_uhs2_irq(host, uhs2mask);

		/* Caller, shdci_irq(), doesn't have to care UHS-2 errors */
		intmask &= ~SDHCI_INT_ERROR;
		mask &= SDHCI_INT_ERROR;
	}

cmd_irq:
	if (intmask & SDHCI_INT_CMD_MASK) {
		/* Clear command interrupt */
		sdhci_writel(host, intmask & SDHCI_INT_CMD_MASK, SDHCI_INT_STATUS);

		/* Handle command interrupt */
		if (intmask & SDHCI_INT_RESPONSE)
			sdhci_uhs2_finish_command(host);

		/* Caller, shdci_irq(), doesn't have to care UHS-2 command */
		intmask &= ~SDHCI_INT_CMD_MASK;
		mask &= SDHCI_INT_CMD_MASK;
	}

	/* Clear already-handled interrupts. */
	sdhci_writel(host, mask, SDHCI_INT_STATUS);

out:
	return intmask;
}
EXPORT_SYMBOL_GPL(sdhci_uhs2_irq);

static irqreturn_t sdhci_uhs2_thread_irq(int irq, void *dev_id)
{
	struct sdhci_host *host = dev_id;
	struct mmc_command *cmd;
	unsigned long flags;
	u32 isr;

	while (!sdhci_uhs2_request_done(host))
		;

	spin_lock_irqsave(&host->lock, flags);

	isr = host->thread_isr;
	host->thread_isr = 0;

	cmd = host->deferred_cmd;
	if (cmd && !sdhci_uhs2_send_command_retry(host, cmd, flags))
		sdhci_finish_mrq(host, cmd->mrq);

	spin_unlock_irqrestore(&host->lock, flags);

	if (isr & (SDHCI_INT_CARD_INSERT | SDHCI_INT_CARD_REMOVE)) {
		struct mmc_host *mmc = host->mmc;

		mmc->ops->card_event(mmc);
		mmc_detect_change(mmc, msecs_to_jiffies(200));
	}

	return IRQ_HANDLED;
}

/*****************************************************************************\
 *
 * Device allocation/registration                                            *
 *                                                                           *
\*****************************************************************************/

static int __sdhci_uhs2_add_host_v4(struct sdhci_host *host, u32 caps1)
{
	struct mmc_host *mmc;
	u32 max_current_caps2;

	if (host->version < SDHCI_SPEC_400)
		return 0;

	mmc = host->mmc;

	/* Support UHS2 */
	if (caps1 & SDHCI_SUPPORT_UHS2)
		mmc->caps2 |= MMC_CAP2_SD_UHS2;

	max_current_caps2 = sdhci_readl(host, SDHCI_MAX_CURRENT_1);

	if ((caps1 & SDHCI_SUPPORT_VDD2_180) &&
	    !max_current_caps2 &&
	    !IS_ERR(mmc->supply.vmmc2)) {
		/* UHS2 - VDD2 */
		int curr = regulator_get_current_limit(mmc->supply.vmmc2);

		if (curr > 0) {
			/* convert to SDHCI_MAX_CURRENT format */
			curr = curr / 1000;  /* convert to mA */
			curr = curr / SDHCI_MAX_CURRENT_MULTIPLIER;
			curr = min_t(u32, curr, SDHCI_MAX_CURRENT_LIMIT);
			max_current_caps2 = curr;
		}
	}

	if (caps1 & SDHCI_SUPPORT_VDD2_180) {
		mmc->ocr_avail_uhs2 |= MMC_VDD2_165_195;
		/*
		 * UHS2 doesn't require this. Only UHS-I bus needs to set
		 * max current.
		 */
		mmc->max_current_180_vdd2 = (max_current_caps2 &
					SDHCI_MAX_CURRENT_VDD2_180_MASK) *
					SDHCI_MAX_CURRENT_MULTIPLIER;
	} else {
		mmc->caps2 &= ~MMC_CAP2_SD_UHS2;
	}

	return 0;
}

static int sdhci_uhs2_host_ops_init(struct sdhci_host *host);

static int __sdhci_uhs2_add_host(struct sdhci_host *host)
{
	unsigned int flags = WQ_UNBOUND | WQ_MEM_RECLAIM | WQ_HIGHPRI;
	struct mmc_host *mmc = host->mmc;
	int ret;

	if ((mmc->caps2 & MMC_CAP2_CQE) &&
	    (host->quirks & SDHCI_QUIRK_BROKEN_CQE)) {
		mmc->caps2 &= ~MMC_CAP2_CQE;
		mmc->cqe_ops = NULL;
	}

	/* overwrite ops */
	if (mmc->caps2 & MMC_CAP2_SD_UHS2)
		sdhci_uhs2_host_ops_init(host);

	host->complete_wq = alloc_workqueue("sdhci", flags, 0);
	if (!host->complete_wq)
		return -ENOMEM;

	INIT_WORK(&host->complete_work, sdhci_uhs2_complete_work);

	timer_setup(&host->timer, sdhci_timeout_timer, 0);
	timer_setup(&host->data_timer, sdhci_timeout_data_timer, 0);

	init_waitqueue_head(&host->buf_ready_int);

	sdhci_init(host, 0);

	ret = request_threaded_irq(host->irq, sdhci_irq,
				   sdhci_uhs2_thread_irq,
				   IRQF_SHARED,	mmc_hostname(mmc), host);
	if (ret) {
		pr_err("%s: Failed to request IRQ %d: %d\n",
		       mmc_hostname(mmc), host->irq, ret);
		goto unwq;
	}

	ret = mmc_add_host(mmc);
		if (ret)
			return 1;

	pr_info("%s: SDHCI controller on %s [%s] using %s\n",
		mmc_hostname(mmc), host->hw_name, dev_name(mmc_dev(mmc)),
		host->use_external_dma ? "External DMA" :
		(host->flags & SDHCI_USE_ADMA) ?
		(host->flags & SDHCI_USE_64_BIT_DMA) ? "ADMA 64-bit" : "ADMA" :
		(host->flags & SDHCI_USE_SDMA) ? "DMA" : "PIO");

	sdhci_enable_card_detection(host);

	return 0;

unwq:
	destroy_workqueue(host->complete_wq);

	return ret;
}

static void __sdhci_uhs2_remove_host(struct sdhci_host *host, int dead)
{
	if (!(host->mmc) || !(host->mmc->flags & MMC_UHS2_SUPPORT))
		return;

	if (!dead)
		host->ops->uhs2_reset(host, SDHCI_UHS2_SW_RESET_FULL);

	sdhci_writel(host, 0, SDHCI_UHS2_ERR_INT_STATUS_EN);
	sdhci_writel(host, 0, SDHCI_UHS2_ERR_INT_SIG_EN);
	host->mmc->flags &= ~MMC_UHS2_INITIALIZED;
}

int sdhci_uhs2_add_host(struct sdhci_host *host)
{
	struct mmc_host *mmc = host->mmc;
	int ret;

	ret = sdhci_setup_host(host);
	if (ret)
		return ret;

	if (host->version >= SDHCI_SPEC_400) {
		ret = __sdhci_uhs2_add_host_v4(host, host->caps1);
		if (ret)
			goto cleanup;
	}

	if ((mmc->caps2 & MMC_CAP2_SD_UHS2) && !host->v4_mode)
		/* host doesn't want to enable UHS2 support */
		/* FIXME: Do we have to do some cleanup here? */
		mmc->caps2 &= ~MMC_CAP2_SD_UHS2;

	ret = __sdhci_uhs2_add_host(host);
	if (ret)
		goto cleanup2;

	return 0;

cleanup2:
	if (host->version >= SDHCI_SPEC_400)
		__sdhci_uhs2_remove_host(host, 0);
cleanup:
	sdhci_cleanup_host(host);

	return ret;
}
EXPORT_SYMBOL_GPL(sdhci_uhs2_add_host);

void sdhci_uhs2_remove_host(struct sdhci_host *host, int dead)
{
	__sdhci_uhs2_remove_host(host, dead);

	sdhci_remove_host(host, dead);
}
EXPORT_SYMBOL_GPL(sdhci_uhs2_remove_host);

void sdhci_uhs2_request(struct mmc_host *mmc, struct mmc_request *mrq)
{
	struct sdhci_host *host = mmc_priv(mmc);
	struct mmc_command *cmd;
	unsigned long flags;
	bool present;

	/* FIXME: check more flags? */
	if (!(sdhci_uhs2_mode(host))) {
		sdhci_request(mmc, mrq);
		return;
	}

	/* Firstly check card presence */
	present = mmc->ops->get_cd(mmc);

	spin_lock_irqsave(&host->lock, flags);

	if (sdhci_present_error(host, mrq->cmd, present))
		goto out_finish;

	cmd = mrq->cmd;

	if (!sdhci_uhs2_send_command(host, cmd))
		goto out_finish;

	spin_unlock_irqrestore(&host->lock, flags);

	return;

out_finish:
	sdhci_finish_mrq(host, mrq);
	spin_unlock_irqrestore(&host->lock, flags);
}
EXPORT_SYMBOL_GPL(sdhci_uhs2_request);

int sdhci_uhs2_request_atomic(struct mmc_host *mmc, struct mmc_request *mrq)
{
	struct sdhci_host *host = mmc_priv(mmc);
	struct mmc_command *cmd;
	unsigned long flags;
	int ret = 0;

	if (!host->mmc->flags & MMC_UHS2_SUPPORT)
		return sdhci_request_atomic(mmc, mrq);

	spin_lock_irqsave(&host->lock, flags);

	if (sdhci_present_error(host, mrq->cmd, true)) {
		sdhci_finish_mrq(host, mrq);
		goto out_finish;
	}

	cmd = mrq->cmd;

	/*
	 * The HSQ may send a command in interrupt context without polling
	 * the busy signaling, which means we should return BUSY if controller
	 * has not released inhibit bits to allow HSQ trying to send request
	 * again in non-atomic context. So we should not finish this request
	 * here.
	 */
	if (!sdhci_uhs2_send_command(host, cmd))
		ret = -EBUSY;

out_finish:
	spin_unlock_irqrestore(&host->lock, flags);
	return ret;
}
EXPORT_SYMBOL_GPL(sdhci_uhs2_request_atomic);

/*****************************************************************************\
 *                                                                           *
 * Driver init/exit                                                          *
 *                                                                           *
\*****************************************************************************/

static int sdhci_uhs2_interface_detect(struct sdhci_host *host)
{
	/* 100ms */
	int timeout = 100000;
	u32 val;

	udelay(200); /* wait for 200us before check */

	if (read_poll_timeout_atomic(sdhci_readl, val, (val & SDHCI_UHS2_IF_DETECT),
				     100, timeout, true, host, SDHCI_PRESENT_STATE)) {
		pr_warn("%s: not detect UHS2 interface in 200us.\n", mmc_hostname(host->mmc));
		sdhci_dumpregs(host);
		return -EIO;
	}

	/* Enable UHS2 error interrupts */
	sdhci_uhs2_clear_set_irqs(host, SDHCI_INT_ALL_MASK,
				  SDHCI_UHS2_ERR_INT_STATUS_MASK);

	/* 150ms */
	timeout = 150000;
	if (read_poll_timeout_atomic(sdhci_readl, val, (val & SDHCI_UHS2_LANE_SYNC),
				     100, timeout, true, host, SDHCI_PRESENT_STATE)) {
		pr_warn("%s: UHS2 Lane sync fail in 150ms.\n", mmc_hostname(host->mmc));
		sdhci_dumpregs(host);
		return -EIO;
	}

	DBG("%s: UHS2 Lane synchronized in UHS2 mode, PHY is initialized.\n",
	    mmc_hostname(host->mmc));
	return 0;
}

static int sdhci_uhs2_init(struct sdhci_host *host)
{
	u16 caps_ptr = 0;
	u32 caps_gen = 0;
	u32 caps_phy = 0;
	u32 caps_tran[2] = {0, 0};
	struct mmc_host *mmc = host->mmc;

	caps_ptr = sdhci_readw(host, SDHCI_UHS2_HOST_CAPS_PTR);
	if (caps_ptr < 0x100 || caps_ptr > 0x1FF) {
		pr_err("%s: SDHCI_UHS2_HOST_CAPS_PTR(%d) is wrong.\n",
		       mmc_hostname(mmc), caps_ptr);
		return -ENODEV;
	}
	caps_gen = sdhci_readl(host,
			       caps_ptr + SDHCI_UHS2_HOST_CAPS_GEN_OFFSET);
	caps_phy = sdhci_readl(host,
			       caps_ptr + SDHCI_UHS2_HOST_CAPS_PHY_OFFSET);
	caps_tran[0] = sdhci_readl(host,
				   caps_ptr + SDHCI_UHS2_HOST_CAPS_TRAN_OFFSET);
	caps_tran[1] = sdhci_readl(host,
				   caps_ptr
					+ SDHCI_UHS2_HOST_CAPS_TRAN_1_OFFSET);

	/* General Caps */
	mmc->uhs2_caps.dap = caps_gen & SDHCI_UHS2_HOST_CAPS_GEN_DAP_MASK;
	mmc->uhs2_caps.gap = (caps_gen & SDHCI_UHS2_HOST_CAPS_GEN_GAP_MASK) >>
			     SDHCI_UHS2_HOST_CAPS_GEN_GAP_SHIFT;
	mmc->uhs2_caps.n_lanes = (caps_gen & SDHCI_UHS2_HOST_CAPS_GEN_LANE_MASK)
			>> SDHCI_UHS2_HOST_CAPS_GEN_LANE_SHIFT;
	mmc->uhs2_caps.addr64 =
		(caps_gen & SDHCI_UHS2_HOST_CAPS_GEN_ADDR_64) ? 1 : 0;
	mmc->uhs2_caps.card_type =
		(caps_gen & SDHCI_UHS2_HOST_CAPS_GEN_DEV_TYPE_MASK) >>
		SDHCI_UHS2_HOST_CAPS_GEN_DEV_TYPE_SHIFT;

	/* PHY Caps */
	mmc->uhs2_caps.phy_rev = caps_phy & SDHCI_UHS2_HOST_CAPS_PHY_REV_MASK;
	mmc->uhs2_caps.speed_range =
		(caps_phy & SDHCI_UHS2_HOST_CAPS_PHY_RANGE_MASK)
		>> SDHCI_UHS2_HOST_CAPS_PHY_RANGE_SHIFT;
	mmc->uhs2_caps.n_lss_sync =
		(caps_phy & SDHCI_UHS2_HOST_CAPS_PHY_N_LSS_SYN_MASK)
		>> SDHCI_UHS2_HOST_CAPS_PHY_N_LSS_SYN_SHIFT;
	mmc->uhs2_caps.n_lss_dir =
		(caps_phy & SDHCI_UHS2_HOST_CAPS_PHY_N_LSS_DIR_MASK)
		>> SDHCI_UHS2_HOST_CAPS_PHY_N_LSS_DIR_SHIFT;
	if (mmc->uhs2_caps.n_lss_sync == 0)
		mmc->uhs2_caps.n_lss_sync = 16 << 2;
	else
		mmc->uhs2_caps.n_lss_sync <<= 2;
	if (mmc->uhs2_caps.n_lss_dir == 0)
		mmc->uhs2_caps.n_lss_dir = 16 << 3;
	else
		mmc->uhs2_caps.n_lss_dir <<= 3;

	/* LINK/TRAN Caps */
	mmc->uhs2_caps.link_rev =
		caps_tran[0] & SDHCI_UHS2_HOST_CAPS_TRAN_LINK_REV_MASK;
	mmc->uhs2_caps.n_fcu =
		(caps_tran[0] & SDHCI_UHS2_HOST_CAPS_TRAN_N_FCU_MASK)
		>> SDHCI_UHS2_HOST_CAPS_TRAN_N_FCU_SHIFT;
	if (mmc->uhs2_caps.n_fcu == 0)
		mmc->uhs2_caps.n_fcu = 256;
	mmc->uhs2_caps.host_type =
		(caps_tran[0] & SDHCI_UHS2_HOST_CAPS_TRAN_HOST_TYPE_MASK)
		>> SDHCI_UHS2_HOST_CAPS_TRAN_HOST_TYPE_SHIFT;
	mmc->uhs2_caps.maxblk_len =
		(caps_tran[0] & SDHCI_UHS2_HOST_CAPS_TRAN_BLK_LEN_MASK)
		>> SDHCI_UHS2_HOST_CAPS_TRAN_BLK_LEN_SHIFT;
	mmc->uhs2_caps.n_data_gap =
		caps_tran[1] & SDHCI_UHS2_HOST_CAPS_TRAN_1_N_DATA_GAP_MASK;

	return 0;
}

static int sdhci_uhs2_do_detect_init(struct mmc_host *mmc)
{
	struct sdhci_host *host = mmc_priv(mmc);
	int ret = -EIO;

	DBG("%s: begin UHS2 init.\n", __func__);

	if (host->ops && host->ops->uhs2_pre_detect_init)
		host->ops->uhs2_pre_detect_init(host);

	if (sdhci_uhs2_interface_detect(host)) {
		pr_warn("%s: cannot detect UHS2 interface.\n",
			mmc_hostname(host->mmc));
		goto out;
	}

	if (sdhci_uhs2_init(host)) {
		pr_warn("%s: UHS2 init fail.\n", mmc_hostname(host->mmc));
		goto out;
	}

	/* Init complete, do soft reset and enable UHS2 error irqs. */
	host->ops->uhs2_reset(host, SDHCI_UHS2_SW_RESET_SD);
	sdhci_uhs2_clear_set_irqs(host, SDHCI_INT_ALL_MASK,
				  SDHCI_UHS2_ERR_INT_STATUS_MASK);
	/*
	 * !!! SDHCI_INT_ENABLE and SDHCI_SIGNAL_ENABLE was cleared
	 * by SDHCI_UHS2_SW_RESET_SD
	 */
	sdhci_writel(host, host->ier, SDHCI_INT_ENABLE);
	sdhci_writel(host, host->ier, SDHCI_SIGNAL_ENABLE);

	ret = 0;
out:
	return ret;
}

static int sdhci_uhs2_host_ops_init(struct sdhci_host *host)
{
	host->mmc_host_ops.start_signal_voltage_switch =
		sdhci_uhs2_start_signal_voltage_switch;
	host->mmc_host_ops.uhs2_set_ios = sdhci_uhs2_set_ios;
	host->mmc_host_ops.uhs2_control = sdhci_uhs2_control;
	host->mmc_host_ops.request = sdhci_uhs2_request;

	if (!host->mmc_host_ops.uhs2_detect_init)
		host->mmc_host_ops.uhs2_detect_init = sdhci_uhs2_do_detect_init;
	if (!host->mmc_host_ops.uhs2_disable_clk)
		host->mmc_host_ops.uhs2_disable_clk = sdhci_uhs2_disable_clk;
	if (!host->mmc_host_ops.uhs2_enable_clk)
		host->mmc_host_ops.uhs2_enable_clk = sdhci_uhs2_enable_clk;

	return 0;
}

static int __init sdhci_uhs2_mod_init(void)
{
	return 0;
}
module_init(sdhci_uhs2_mod_init);

static void __exit sdhci_uhs2_exit(void)
{
}
module_exit(sdhci_uhs2_exit);

MODULE_AUTHOR("Intel, Genesys Logic, Linaro");
MODULE_DESCRIPTION("MMC UHS-II Support");
MODULE_LICENSE("GPL v2");
