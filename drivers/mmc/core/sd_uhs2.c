// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2021 Linaro Ltd
 * Author: Ulf Hansson <ulf.hansson@linaro.org>
 *
 * Copyright (C) 2014 Intel Corp, All Rights Reserved.
 * Author: Yi Sun <yi.y.sun@intel.com>
 *
 * Copyright (C) 2020 Genesys Logic, Inc.
 * Authors: Ben Chuang <ben.chuang@genesyslogic.com.tw>
 *
 * Copyright (C) 2020 Linaro Limited
 * Author: AKASHI Takahiro <takahiro.akashi@linaro.org>
 *
 * Copyright (C) 2022 Genesys Logic, Inc.
 * Authors: Jason Lai <jason.lai@genesyslogic.com.tw>
 *
 * Support for SD UHS-II cards
 */
#include <linux/err.h>
#include <linux/pm_runtime.h>

#include <linux/mmc/host.h>
#include <linux/mmc/card.h>
#include <linux/mmc/mmc.h>
#include <linux/mmc/sd.h>
#include <linux/mmc/sd_uhs2.h>

#include "card.h"
#include "core.h"
#include "bus.h"
#include "sd.h"
#include "sd_ops.h"
#include "mmc_ops.h"

#define UHS2_WAIT_CFG_COMPLETE_PERIOD_US	(1 * 1000)	/* 1ms */
#define UHS2_WAIT_CFG_COMPLETE_TIMEOUT_MS	100		/* 100ms */

static const unsigned int sd_uhs2_freqs[] = { 52000000, 26000000 };
int sd_uhs2_reinit(struct mmc_host *host);

/*
 * Internal function that does the actual ios call to the host driver,
 * optionally printing some debug output.
 */
static inline int sd_uhs2_set_ios(struct mmc_host *host)
{
	struct mmc_ios *ios = &host->ios;

	pr_debug("%s: clock %uHz powermode %u Vdd %u timing %u\n",
		 mmc_hostname(host), ios->clock, ios->power_mode, ios->vdd, ios->timing);

	return host->ops->uhs2_set_ios(host, ios);
}

static int sd_uhs2_power_up(struct mmc_host *host)
{
	int err;

	if (host->ios.power_mode == MMC_POWER_ON)
		return 0;

	host->ios.vdd = fls(host->ocr_avail) - 1;
	host->ios.clock = host->f_init;
	host->ios.timing = MMC_TIMING_SD_UHS2;
	host->ios.power_mode = MMC_POWER_ON;

	err = sd_uhs2_set_ios(host);

	mmc_delay(host->uhs2_ios.power_delay_ms);

	return err;
}

static int sd_uhs2_power_off(struct mmc_host *host)
{
	if (host->ios.power_mode == MMC_POWER_OFF)
		return 0;

	host->ios.vdd = 0;
	host->ios.clock = 0;
	host->ios.timing = MMC_TIMING_LEGACY;
	host->ios.power_mode = MMC_POWER_OFF;

	return sd_uhs2_set_ios(host);
}

/**
 * sd_uhs2_cmd_assemble() - build up UHS-II command packet which is embedded in
 *                          mmc_command structure
 * @cmd:	MMC command to executed
 * @uhs2_cmd:	UHS2 command corresponded to MMC command
 * @header:	Header field of UHS-II command cxpacket
 * @arg:	Argument field of UHS-II command packet
 * @payload:	Payload field of UHS-II command packet
 * @plen:	Payload length
 * @resp:	Response buffer is allocated by caller and it is used to keep
 *              the response of CM-TRAN command. For SD-TRAN command, uhs2_resp
 *              should be null and SD-TRAN command response should be stored in
 *              resp of mmc_command.
 * @resp_len:	Response buffer length
 *
 * The uhs2_command structure contains message packets which are transmited/
 * received on UHS-II bus. This function fills in the contents of uhs2_command
 * structure and embededs UHS2 command into mmc_command structure, which is used
 * in legacy SD operation functions.
 *
 */
static void sd_uhs2_cmd_assemble(struct mmc_command *cmd,
				 struct uhs2_command *uhs2_cmd,
				 u16 header, u16 arg, __be32 *payload,
				 u8 plen, u8 *resp, u8 resp_len)
{
	uhs2_cmd->header = header;
	uhs2_cmd->arg = arg;
	uhs2_cmd->payload = payload;
	uhs2_cmd->payload_len = plen * sizeof(u32);
	uhs2_cmd->packet_len = uhs2_cmd->payload_len + 4;

	cmd->uhs2_cmd = uhs2_cmd;
	cmd->uhs2_resp = resp;
	cmd->uhs2_resp_len = resp_len;
}

/*
 * Run the phy initialization sequence, which mainly relies on the UHS-II host
 * to check that we reach the expected electrical state, between the host and
 * the card.
 */
static int sd_uhs2_phy_init(struct mmc_host *host)
{
	int err = 0;

	err = host->ops->uhs2_control(host, UHS2_PHY_INIT);
	if (err) {
		pr_err("%s: failed to initial phy for UHS-II!\n",
		       mmc_hostname(host));
	}

	return err;
}

/*
 * Do the early initialization of the card, by sending the device init broadcast
 * command and wait for the process to be completed.
 */
static int sd_uhs2_dev_init(struct mmc_host *host)
{
	struct mmc_command cmd = {0};
	struct uhs2_command uhs2_cmd = {};
	u32 cnt;
	u32 dap, gap, resp_gap;
	u16 header, arg;
	__be32 payload[UHS2_DEV_INIT_PAYLOAD_LEN];
	u8 gd = 0;
	u8 resp[UHS2_DEV_ENUM_RESP_LEN] = {0};
	int err;

	dap = host->uhs2_caps.dap;
	gap = host->uhs2_caps.gap;

	/*
	 * Refer to UHS-II Addendum Version 1.02 Figure 6-21 to see DEVICE_INIT CCMD format.
	 * Head:
	 *      - Control Write(R/W=1) with 4-Byte payload(PLEN=01b).
	 *      - IOADR = CMD_BASE + 002h
	 * Payload:
	 *      - bit [3:0]  : GAP(Group Allocated Power)
	 *      - bit [7:4]  : GD(Group Descriptor)
	 *      - bit [11]   : Complete Flag
	 *      - bit [15:12]: DAP(Device Allocated Power)
	 */
	header = UHS2_NATIVE_PACKET | UHS2_PACKET_TYPE_CCMD;
	arg = ((UHS2_DEV_CMD_DEVICE_INIT & 0xFF) << 8) |
	       UHS2_NATIVE_CMD_WRITE |
	       UHS2_NATIVE_CMD_PLEN_4B |
	       (UHS2_DEV_CMD_DEVICE_INIT >> 8);

	/*
	 * Refer to UHS-II Addendum Version 1.02 section 6.3.1.
	 * Max. time from DEVICE_INIT CCMD EOP reception on Device
	 * Rx to its SOP transmission on Device Tx(Tfwd_init_cmd) is
	 * 1 second.
	 */
	cmd.busy_timeout = 1000;

	/*
	 * Refer to UHS-II Addendum Version 1.02 section 6.2.6.3.
	 * When the number of the DEVICE_INIT commands is reach to
	 * 30 tiems, Host shall stop issuing DEVICE_INIT command
	 * and regard it as an error.
	 */
	for (cnt = 0; cnt < 30; cnt++) {
		payload[0] = ((dap & 0xF) << 12) |
			      UHS2_DEV_INIT_COMPLETE_FLAG |
			      ((gd & 0xF) << 4) |
			      (gap & 0xF);

		sd_uhs2_cmd_assemble(&cmd, &uhs2_cmd, header, arg,
				     payload, UHS2_DEV_INIT_PAYLOAD_LEN,
				     resp, UHS2_DEV_INIT_RESP_LEN);

		err = mmc_wait_for_cmd(host, &cmd, 0);

		if (err) {
			pr_err("%s: %s: UHS2 CMD send fail, err= 0x%x!\n",
			       mmc_hostname(host), __func__, err);
			return err;
		}

		if (resp[3] != (UHS2_DEV_CMD_DEVICE_INIT & 0xFF)) {
			pr_err("%s: DEVICE_INIT response is wrong!\n",
			       mmc_hostname(host));
			return -EIO;
		}

		if (resp[5] & 0x8) {
			host->uhs2_caps.group_desc = gd;
			return 0;
		}
		resp_gap = resp[4] & 0x0F;
		if (gap == resp_gap)
			gd++;
	}
	if (cnt == 30) {
		pr_err("%s: DEVICE_INIT fail, already 30 times!\n",
		       mmc_hostname(host));
		return -EIO;
	}

	return 0;
}

/*
 * Run the enumeration process by sending the enumerate command to the card.
 * Note that, we currently support only the point to point connection, which
 * means only one card can be attached per host/slot.
 */
static int sd_uhs2_enum(struct mmc_host *host, u32 *node_id)
{
	struct mmc_command cmd = {0};
	struct uhs2_command uhs2_cmd = {};
	u16 header, arg;
	__be32 payload[UHS2_DEV_ENUM_PAYLOAD_LEN];
	u8 id_f = 0xF, id_l = 0x0;
	u8 resp[UHS2_DEV_ENUM_RESP_LEN] = {0};
	int err;

	/*
	 * Refer to UHS-II Addendum Version 1.02 Figure 6-28 to see ENUMERATE CCMD format.
	 * Header:
	 *      - Control Write(R/W=1) with 4-Byte payload(PLEN=01b).
	 *      - IOADR = CMD_BASE + 003h
	 * Payload:
	 *      - bit [3:0]: ID_L(Last Node ID)
	 *      - bit [7:4]: ID_F(First Node ID)
	 */
	header = UHS2_NATIVE_PACKET | UHS2_PACKET_TYPE_CCMD;
	arg = ((UHS2_DEV_CMD_ENUMERATE & 0xFF) << 8) |
	       UHS2_NATIVE_CMD_WRITE |
	       UHS2_NATIVE_CMD_PLEN_4B |
	       (UHS2_DEV_CMD_ENUMERATE >> 8);

	payload[0] = (id_f << 4) | id_l;
	payload[0] = cpu_to_be32(payload[0]);

	sd_uhs2_cmd_assemble(&cmd, &uhs2_cmd, header, arg, payload, UHS2_DEV_ENUM_PAYLOAD_LEN,
			     resp, UHS2_DEV_ENUM_RESP_LEN);

	err = mmc_wait_for_cmd(host, &cmd, 0);
	if (err) {
		pr_err("%s: %s: UHS2 CMD send fail, err= 0x%x!\n",
		       mmc_hostname(host), __func__, err);
		return err;
	}

	if (resp[3] != (UHS2_DEV_CMD_ENUMERATE & 0xFF)) {
		pr_err("%s: ENUMERATE response is wrong!\n",
		       mmc_hostname(host));
		return -EIO;
	}

	id_f = (resp[4] >> 4) & 0xF;
	id_l = resp[4] & 0xF;
	*node_id = id_f;

	return 0;
}

/*
 * Read the UHS-II configuration registers (CFG_REG) of the card, by sending it
 * commands and by parsing the responses. Store a copy of the relevant data in
 * card->uhs2_config.
 */
static int sd_uhs2_config_read(struct mmc_host *host, struct mmc_card *card)
{
	struct mmc_command cmd = {0};
	struct uhs2_command uhs2_cmd = {};
	u16 header, arg;
	u32 cap;
	int err;

	/*
	 * Use Control Read CCMD to read Generic Capability from Configuration Register.
	 * - Control Write(R/W=1) with 4-Byte payload(PLEN=01b).
	 * - IOADR = Generic Capability Register(CFG_BASE + 000h)
	 */
	header = UHS2_NATIVE_PACKET | UHS2_PACKET_TYPE_CCMD | card->uhs2_config.node_id;
	arg = ((UHS2_DEV_CONFIG_GEN_CAPS & 0xFF) << 8) |
	       UHS2_NATIVE_CMD_READ |
	       UHS2_NATIVE_CMD_PLEN_4B |
	       (UHS2_DEV_CONFIG_GEN_CAPS >> 8);

	/*
	 * There is no payload because per spec, there should be
	 * no payload field for read CCMD.
	 * Plen is set in arg. Per spec, plen for read CCMD
	 * represents the len of read data which is assigned in payload
	 * of following RES (p136).
	 */
	sd_uhs2_cmd_assemble(&cmd, &uhs2_cmd, header, arg, NULL, 0, NULL, 0);

	err = mmc_wait_for_cmd(host, &cmd, 0);
	if (err) {
		pr_err("%s: %s: UHS2 CMD send fail, err= 0x%x!\n",
		       mmc_hostname(host), __func__, err);
		return err;
	}

	/*
	 * Generic Capability Register:
	 * bit [7:0]  : Reserved
	 * bit [13:8] : Device-Specific Number of Lanes and Functionality
	 *              bit 8: 2L-HD
	 *              bit 9: 2D-1U FD
	 *              bit 10: 1D-2U FD
	 *              bit 11: 2D-2U FD
	 *              Others: Reserved
	 * bit [14]   : DADR Length
	 *              0: 4 bytes
	 *              1: Reserved
	 * bit [23:16]: Application Type
	 *              bit 16: 0=Non-SD memory, 1=SD memory
	 *              bit 17: 0=Non-SDIO, 1=SDIO
	 *              bit 18: 0=Card, 1=Embedded
	 * bit [63:24]: Reserved
	 */
	cap = cmd.resp[0];
	card->uhs2_config.n_lanes =
				(cap >> UHS2_DEV_CONFIG_N_LANES_POS) &
				UHS2_DEV_CONFIG_N_LANES_MASK;
	card->uhs2_config.dadr_len =
				(cap >> UHS2_DEV_CONFIG_DADR_POS) &
				UHS2_DEV_CONFIG_DADR_MASK;
	card->uhs2_config.app_type =
				(cap >> UHS2_DEV_CONFIG_APP_POS) &
				UHS2_DEV_CONFIG_APP_MASK;

	/*
	 * Use Control Read CCMD to read PHY Capability from Configuration Register.
	 * - Control Write(R/W=1) with 8-Byte payload(PLEN=10b).
	 * - IOADR = PHY Capability Register(CFG_BASE + 002h)
	 */
	arg = ((UHS2_DEV_CONFIG_PHY_CAPS & 0xFF) << 8) |
	       UHS2_NATIVE_CMD_READ |
	       UHS2_NATIVE_CMD_PLEN_8B |
	      (UHS2_DEV_CONFIG_PHY_CAPS >> 8);

	sd_uhs2_cmd_assemble(&cmd, &uhs2_cmd, header, arg, NULL, 0, NULL, 0);

	err = mmc_wait_for_cmd(host, &cmd, 0);
	if (err) {
		pr_err("%s: %s: UHS2 CMD send fail, err= 0x%x!\n",
		       mmc_hostname(host), __func__, err);
		return err;
	}

	/*
	 * PHY Capability Register:
	 * bit [3:0]  : PHY Minor Revision
	 * bit [5:4]  : PHY Major Revision
	 * bit [15]   : Support Hibernate Mode
	 *              0: Not support Hibernate Mode
	 *              1: Support Hibernate Mode
	 * bit [31:16]: Reserved
	 * bit [35:32]: Device-Specific N_LSS_SYN
	 * bit [39:36]: Device-Specific N_LSS_DIR
	 * bit [63:40]: Reserved
	 */
	cap = cmd.resp[0];
	card->uhs2_config.phy_minor_rev =
				cap & UHS2_DEV_CONFIG_PHY_MINOR_MASK;
	card->uhs2_config.phy_major_rev =
				(cap >> UHS2_DEV_CONFIG_PHY_MAJOR_POS) &
				 UHS2_DEV_CONFIG_PHY_MAJOR_MASK;
	card->uhs2_config.can_hibernate =
				(cap >> UHS2_DEV_CONFIG_CAN_HIBER_POS) &
				 UHS2_DEV_CONFIG_CAN_HIBER_MASK;

	cap = cmd.resp[1];
	card->uhs2_config.n_lss_sync =
				cap & UHS2_DEV_CONFIG_N_LSS_SYN_MASK;
	card->uhs2_config.n_lss_dir =
				(cap >> UHS2_DEV_CONFIG_N_LSS_DIR_POS) &
				UHS2_DEV_CONFIG_N_LSS_DIR_MASK;
	if (card->uhs2_config.n_lss_sync == 0)
		card->uhs2_config.n_lss_sync = 16 << 2;
	else
		card->uhs2_config.n_lss_sync <<= 2;

	if (card->uhs2_config.n_lss_dir == 0)
		card->uhs2_config.n_lss_dir = 16 << 3;
	else
		card->uhs2_config.n_lss_dir <<= 3;

	/*
	 * Use Control Read CCMD to read LINK/TRAN Capability from Configuration Register.
	 * - Control Write(R/W=1) with 8-Byte payload(PLEN=10b).
	 * - IOADR = LINK/TRAN Capability Register(CFG_BASE + 004h)
	 */
	arg = ((UHS2_DEV_CONFIG_LINK_TRAN_CAPS & 0xFF) << 8) |
		UHS2_NATIVE_CMD_READ |
		UHS2_NATIVE_CMD_PLEN_8B |
		(UHS2_DEV_CONFIG_LINK_TRAN_CAPS >> 8);

	sd_uhs2_cmd_assemble(&cmd, &uhs2_cmd, header, arg, NULL, 0, NULL, 0);

	err = mmc_wait_for_cmd(host, &cmd, 0);
	if (err) {
		pr_err("%s: %s: UHS2 CMD send fail, err= 0x%x!\n",
		       mmc_hostname(host), __func__, err);
		return err;
	}

	/*
	 * LINK/TRAN Capability Register:
	 * bit [3:0]  : LINK_TRAN Minor Revision
	 * bit [5:4]  : LINK/TRAN Major Revision
	 * bit [7:6]  : Reserved
	 * bit [15:8] : Device-Specific N_FCU
	 * bit [18:16]: Device Type
	 *              001b=Host
	 *              010b=Device
	 *              011b=Reserved for CMD issuable Device
	 * bit [19]   : Reserved
	 * bit [31:20]: Device-Specific MAX_BLKLEN
	 * bit [39:32]: Device-Specific N_DATA_GAP
	 * bit [63:40]: Reserved
	 */
	cap = cmd.resp[0];
	card->uhs2_config.link_minor_rev =
				cap & UHS2_DEV_CONFIG_LT_MINOR_MASK;
	card->uhs2_config.link_major_rev =
				(cap >> UHS2_DEV_CONFIG_LT_MAJOR_POS) &
				UHS2_DEV_CONFIG_LT_MAJOR_MASK;
	card->uhs2_config.n_fcu =
				(cap >> UHS2_DEV_CONFIG_N_FCU_POS) &
				UHS2_DEV_CONFIG_N_FCU_MASK;
	card->uhs2_config.dev_type =
				(cap >> UHS2_DEV_CONFIG_DEV_TYPE_POS) &
				UHS2_DEV_CONFIG_DEV_TYPE_MASK;
	card->uhs2_config.maxblk_len =
				(cap >> UHS2_DEV_CONFIG_MAX_BLK_LEN_POS) &
				UHS2_DEV_CONFIG_MAX_BLK_LEN_MASK;

	cap = cmd.resp[1];
	card->uhs2_config.n_data_gap =
				cap & UHS2_DEV_CONFIG_N_DATA_GAP_MASK;
	if (card->uhs2_config.n_fcu == 0)
		card->uhs2_config.n_fcu = 256;

	return 0;
}

/*
 * Based on the card's and host's UHS-II capabilities, let's update the
 * configuration of the card and the host. This may also include to move to a
 * greater speed range/mode. Depending on the updated configuration, we may need
 * to do a soft reset of the card via sending it a GO_DORMANT_STATE command.
 *
 * In the final step, let's check if the card signals "config completion", which
 * indicates that the card has moved from config state into active state.
 */
static int sd_uhs2_config_write(struct mmc_host *host, struct mmc_card *card)
{
	struct mmc_command cmd = {0};
	struct uhs2_command uhs2_cmd = {};
	u16 header, arg;
	__be32 payload[UHS2_CFG_WRITE_PAYLOAD_LEN];
	u8 nMinDataGap;
	int err;
	u8 resp[5] = {0};

	/*
	 * Use Control Write CCMD to set Generic Setting in Configuration Register.
	 * - Control Write(R/W=1) with 8-Byte payload(PLEN=10b).
	 * - IOADR = Generic Setting Register(CFG_BASE + 008h)
	 * - Payload = New contents to be written to Generic Setting Register
	 */
	header = UHS2_NATIVE_PACKET | UHS2_PACKET_TYPE_CCMD | card->uhs2_config.node_id;
	arg = ((UHS2_DEV_CONFIG_GEN_SET & 0xFF) << 8) |
	       UHS2_NATIVE_CMD_WRITE |
	       UHS2_NATIVE_CMD_PLEN_8B |
	       (UHS2_DEV_CONFIG_GEN_SET >> 8);

	if (card->uhs2_config.n_lanes == UHS2_DEV_CONFIG_2L_HD_FD &&
	    host->uhs2_caps.n_lanes == UHS2_DEV_CONFIG_2L_HD_FD) {
		/* Support HD */
		host->uhs2_ios.is_2L_HD_mode = true;
		nMinDataGap = 1;
	} else {
		/* Only support 2L-FD so far */
		host->uhs2_ios.is_2L_HD_mode = false;
		nMinDataGap = 3;
	}

	/*
	 * Most UHS-II cards only support FD and 2L-HD mode. Other lane numbers
	 * defined in UHS-II addendem Ver1.01 are optional.
	 */
	host->uhs2_caps.n_lanes_set = UHS2_DEV_CONFIG_GEN_SET_2L_FD_HD;
	card->uhs2_config.n_lanes_set = UHS2_DEV_CONFIG_GEN_SET_2L_FD_HD;

	payload[0] = card->uhs2_config.n_lanes_set << UHS2_DEV_CONFIG_N_LANES_POS;
	payload[1] = 0;
	payload[0] = cpu_to_be32(payload[0]);
	payload[1] = cpu_to_be32(payload[1]);

	/*
	 * There is no payload because per spec, there should be
	 * no payload field for read CCMD.
	 * Plen is set in arg. Per spec, plen for read CCMD
	 * represents the len of read data which is assigned in payload
	 * of following RES (p136).
	 */
	sd_uhs2_cmd_assemble(&cmd, &uhs2_cmd, header, arg, payload, UHS2_CFG_WRITE_PAYLOAD_LEN,
			     NULL, 0);

	err = mmc_wait_for_cmd(host, &cmd, 0);
	if (err) {
		pr_err("%s: %s: UHS2 CMD send fail, err= 0x%x!\n",
		       mmc_hostname(host), __func__, err);
		return err;
	}

	/*
	 * Use Control Write CCMD to set PHY Setting in Configuration Register.
	 * - Control Write(R/W=1) with 8-Byte payload(PLEN=10b).
	 * - IOADR = PHY Setting Register(CFG_BASE + 00Ah)
	 * - Payload = New contents to be written to PHY Setting Register
	 */
	arg = ((UHS2_DEV_CONFIG_PHY_SET & 0xFF) << 8) |
	       UHS2_NATIVE_CMD_WRITE |
	       UHS2_NATIVE_CMD_PLEN_8B |
	       (UHS2_DEV_CONFIG_PHY_SET >> 8);

	if (host->uhs2_caps.speed_range == UHS2_DEV_CONFIG_PHY_SET_SPEED_B) {
		card->uhs2_state |= MMC_UHS2_SPEED_B;
		card->uhs2_config.speed_range_set =
					UHS2_DEV_CONFIG_PHY_SET_SPEED_B;
	} else {
		card->uhs2_config.speed_range_set = UHS2_DEV_CONFIG_PHY_SET_SPEED_A;
		card->uhs2_state &= ~MMC_UHS2_SPEED_B;
	}

	payload[0] = card->uhs2_config.speed_range_set << UHS2_DEV_CONFIG_PHY_SET_SPEED_POS;

	card->uhs2_config.n_lss_sync_set = (max(card->uhs2_config.n_lss_sync,
						host->uhs2_caps.n_lss_sync) >> 2) &
					   UHS2_DEV_CONFIG_N_LSS_SYN_MASK;
	host->uhs2_caps.n_lss_sync_set = card->uhs2_config.n_lss_sync_set;

	card->uhs2_config.n_lss_dir_set = (max(card->uhs2_config.n_lss_dir,
					       host->uhs2_caps.n_lss_dir) >> 3) &
					  UHS2_DEV_CONFIG_N_LSS_DIR_MASK;
	host->uhs2_caps.n_lss_dir_set = card->uhs2_config.n_lss_dir_set;

	payload[1] = (card->uhs2_config.n_lss_dir_set << UHS2_DEV_CONFIG_N_LSS_DIR_POS) |
		     card->uhs2_config.n_lss_sync_set;
	payload[0] = cpu_to_be32(payload[0]);
	payload[1] = cpu_to_be32(payload[1]);

	memset(resp, 0, sizeof(resp));

	sd_uhs2_cmd_assemble(&cmd, &uhs2_cmd, header, arg, payload, UHS2_CFG_WRITE_PAYLOAD_LEN,
			     resp, UHS2_CFG_WRITE_PHY_SET_RESP_LEN);

	err = mmc_wait_for_cmd(host, &cmd, 0);
	if (err) {
		pr_err("%s: %s: UHS2 CMD send fail, err= 0x%x!\n",
		       mmc_hostname(host), __func__, err);
		return err;
	}

	if ((resp[2] & 0x80)) {
		pr_err("%s: %s: UHS2 CMD not accepted, resp= 0x%x!\n",
		       mmc_hostname(host), __func__, resp[2]);
		return -EIO;
	}

	/*
	 * Use Control Write CCMD to set LINK/TRAN Setting in Configuration Register.
	 * - Control Write(R/W=1) with 8-Byte payload(PLEN=10b).
	 * - IOADR = LINK/TRAN Setting Register(CFG_BASE + 00Ch)
	 * - Payload = New contents to be written to LINK/TRAN Setting Register
	 */
	arg = ((UHS2_DEV_CONFIG_LINK_TRAN_SET & 0xFF) << 8) |
		UHS2_NATIVE_CMD_WRITE |
		UHS2_NATIVE_CMD_PLEN_8B |
		(UHS2_DEV_CONFIG_LINK_TRAN_SET >> 8);

	if (card->uhs2_config.app_type == UHS2_DEV_CONFIG_APP_SD_MEM)
		card->uhs2_config.maxblk_len_set = UHS2_DEV_CONFIG_LT_SET_MAX_BLK_LEN;
	else
		card->uhs2_config.maxblk_len_set = min(card->uhs2_config.maxblk_len,
						       host->uhs2_caps.maxblk_len);
	host->uhs2_caps.maxblk_len_set = card->uhs2_config.maxblk_len_set;

	card->uhs2_config.n_fcu_set = min(card->uhs2_config.n_fcu, host->uhs2_caps.n_fcu);
	host->uhs2_caps.n_fcu_set = card->uhs2_config.n_fcu_set;

	card->uhs2_config.n_data_gap_set = max(nMinDataGap, card->uhs2_config.n_data_gap);
	host->uhs2_caps.n_data_gap_set = card->uhs2_config.n_data_gap_set;

	host->uhs2_caps.max_retry_set = 3;
	card->uhs2_config.max_retry_set = host->uhs2_caps.max_retry_set;

	payload[0] = (card->uhs2_config.maxblk_len_set << UHS2_DEV_CONFIG_MAX_BLK_LEN_POS) |
		     (card->uhs2_config.max_retry_set << UHS2_DEV_CONFIG_LT_SET_MAX_RETRY_POS) |
		     (card->uhs2_config.n_fcu_set << UHS2_DEV_CONFIG_N_FCU_POS);
	payload[1] = card->uhs2_config.n_data_gap_set;
	payload[0] = cpu_to_be32(payload[0]);
	payload[1] = cpu_to_be32(payload[1]);

	sd_uhs2_cmd_assemble(&cmd, &uhs2_cmd, header, arg, payload, UHS2_CFG_WRITE_PAYLOAD_LEN,
			     NULL, 0);

	err = mmc_wait_for_cmd(host, &cmd, 0);
	if (err) {
		pr_err("%s: %s: UHS2 CMD send fail, err= 0x%x!\n",
		       mmc_hostname(host), __func__, err);
		return err;
	}

	/*
	 * Use Control Write CCMD to set Config Completion(payload bit 63) in Generic Setting
	 * Register.
	 * Header:
	 *      - Control Write(R/W=1) with 8-Byte payload(PLEN=10b).
	 *      - IOADR = PGeneric Setting Register(CFG_BASE + 008h)
	 * Payload:
	 *      - bit [63]: Config Completion
	 *
	 * DLSM transits to Active state immediately when Config Completion is set to 1.
	 */
	arg = ((UHS2_DEV_CONFIG_GEN_SET & 0xFF) << 8) |
	       UHS2_NATIVE_CMD_WRITE |
	       UHS2_NATIVE_CMD_PLEN_8B |
	       (UHS2_DEV_CONFIG_GEN_SET >> 8);

	payload[0] = 0;
	payload[1] = UHS2_DEV_CONFIG_GEN_SET_CFG_COMPLETE;
	payload[0] = cpu_to_be32(payload[0]);
	payload[1] = cpu_to_be32(payload[1]);

	memset(resp, 0, sizeof(resp));
	sd_uhs2_cmd_assemble(&cmd, &uhs2_cmd, header, arg, payload, UHS2_CFG_WRITE_PAYLOAD_LEN,
			     resp, UHS2_CFG_WRITE_GENERIC_SET_RESP_LEN);

	err = mmc_wait_for_cmd(host, &cmd, 0);
	if (err) {
		pr_err("%s: %s: UHS2 CMD send fail, err= 0x%x!\n",
		       mmc_hostname(host), __func__, err);
		return err;
	}

	/* Set host Config Setting registers */
	err = host->ops->uhs2_control(host, UHS2_SET_CONFIG);
	if (err) {
		pr_err("%s: %s: UHS2 SET_CONFIG fail!\n", mmc_hostname(host), __func__);
		return err;
	}

	return 0;
}

static int sd_uhs2_go_dormant(struct mmc_host *host, u32 node_id)
{
	struct mmc_command cmd = {0};
	struct uhs2_command uhs2_cmd = {};
	u16 header, arg;
	__be32 payload[1];
	int err;

	/* Disable Normal INT */
	err = host->ops->uhs2_control(host, UHS2_DISABLE_INT);
	if (err) {
		pr_err("%s: %s: UHS2 DISABLE_INT fail!\n",
		       mmc_hostname(host), __func__);
		return err;
	}

	/*
	 * Refer to UHS-II Addendum Version 1.02 Figure 6-17 to see GO_DORMANT_STATE CCMD format.
	 * Header:
	 *      - Control Write(R/W=1) with 4-Byte payload(PLEN=01b).
	 *      - IOADR = CMD_BASE + 001h
	 * Payload:
	 *      - bit [7]: HBR(Entry to Hibernate Mode)
	 *                 1: Host intends to enter Hibernate mode during Dormant state.
	 *                 The default setting is 0 because hibernate is currently not supported.
	 */
	header = UHS2_NATIVE_PACKET | UHS2_PACKET_TYPE_CCMD | node_id;
	arg = ((UHS2_DEV_CMD_GO_DORMANT_STATE & 0xFF) << 8) |
		UHS2_NATIVE_CMD_WRITE |
		UHS2_NATIVE_CMD_PLEN_4B |
		(UHS2_DEV_CMD_GO_DORMANT_STATE >> 8);

	sd_uhs2_cmd_assemble(&cmd, &uhs2_cmd, header, arg, payload, UHS2_GO_DORMANT_PAYLOAD_LEN,
			     NULL, 0);

	err = mmc_wait_for_cmd(host, &cmd, 0);
	if (err) {
		pr_err("%s: %s: UHS2 CMD send fail, err= 0x%x!\n",
		       mmc_hostname(host), __func__, err);
		return err;
	}

	/* Check Dormant State in Present */
	err = host->ops->uhs2_control(host, UHS2_CHECK_DORMANT);
	if (err)
		return err;

	/* Disable UHS2 card clock */
	err = host->ops->uhs2_control(host, UHS2_DISABLE_CLK);
	if (err)
		return err;

	/* Restore sd clock */
	mmc_delay(5);
	err = host->ops->uhs2_control(host, UHS2_ENABLE_CLK);
	if (err)
		return err;

	/* Enable Normal INT */
	err = host->ops->uhs2_control(host, UHS2_ENABLE_INT);
	if (err)
		return err;

	/* Detect UHS2 */
	err = host->ops->uhs2_control(host, UHS2_PHY_INIT);
	if (err)
		return err;

	return 0;
}

static int __sd_uhs2_wait_active_state_cb(void *cb_data, bool *busy)
{
	struct sd_uhs2_wait_active_state_data *data = cb_data;
	struct mmc_host *host = data->host;
	struct mmc_command *cmd = data->cmd;
	int err;

	err = mmc_wait_for_cmd(host, cmd, 0);
	if (err)
		return err;

	if (cmd->resp[1] & UHS2_DEV_CONFIG_GEN_SET_CFG_COMPLETE)
		*busy = false;
	else
		*busy = true;

	return 0;
}

static int sd_uhs2_change_speed(struct mmc_host *host, u32 node_id)
{
	struct mmc_command cmd = {0};
	struct uhs2_command uhs2_cmd = {};
	u16 header, arg;
	int err;
	struct sd_uhs2_wait_active_state_data cb_data = {
		.host = host,
		.cmd = &cmd
	};

	/* Change Speed Range at controller side. */
	err = host->ops->uhs2_control(host, UHS2_SET_SPEED_B);
	if (err) {
		pr_err("%s: %s: UHS2 SET_SPEED fail!\n", mmc_hostname(host), __func__);
		return err;
	}

	err = sd_uhs2_go_dormant(host, node_id);
	if (err) {
		pr_err("%s: %s: UHS2 GO_DORMANT_STATE fail, err= 0x%x!\n",
		       mmc_hostname(host), __func__, err);
		return err;
	}

	/*
	 * Use Control Read CCMD to check Config Completion(bit 63) in Generic Setting Register.
	 * - Control Read(R/W=0) with 8-Byte payload(PLEN=10b).
	 * - IOADR = Generic Setting Register(CFG_BASE + 008h)
	 *
	 * When UHS-II card been switched to new speed mode, it will set Config Completion to 1.
	 */
	header = UHS2_NATIVE_PACKET | UHS2_PACKET_TYPE_CCMD | node_id;
	arg = ((UHS2_DEV_CONFIG_GEN_SET & 0xFF) << 8) |
		UHS2_NATIVE_CMD_READ |
		UHS2_NATIVE_CMD_PLEN_8B |
		(UHS2_DEV_CONFIG_GEN_SET >> 8);

	sd_uhs2_cmd_assemble(&cmd, &uhs2_cmd, header, arg, NULL, 0, NULL, 0);
	err = __mmc_poll_for_busy(host, UHS2_WAIT_CFG_COMPLETE_PERIOD_US,
				  UHS2_WAIT_CFG_COMPLETE_TIMEOUT_MS,
				  &__sd_uhs2_wait_active_state_cb, &cb_data);
	if (err) {
		pr_err("%s: %s: Not switch to Active in 100 ms\n", mmc_hostname(host), __func__);
		return err;
	}

	return 0;
}

static int sd_uhs2_get_ro(struct mmc_host *host)
{
	/*
	 * Some systems don't feature a write-protect pin and don't need one.
	 * E.g. because they only have micro-SD card slot. For those systems
	 * assume that the SD card is always read-write.
	 */
	if (host->caps2 & MMC_CAP2_NO_WRITE_PROTECT)
		return 0;

	if (!host->ops->get_ro)
		return -1;

	return host->ops->get_ro(host);
}

/*
 * Mask off any voltages we don't support and select
 * the lowest voltage
 */
u32 sd_uhs2_select_voltage(struct mmc_host *host, u32 ocr)
{
	int bit;
	int err;

	/*
	 * Sanity check the voltages that the card claims to
	 * support.
	 */
	if (ocr & 0x7F) {
		dev_warn(mmc_dev(host), "card claims to support voltages below defined range\n");
		ocr &= ~0x7F;
	}

	ocr &= host->ocr_avail;
	if (!ocr) {
		dev_warn(mmc_dev(host), "no support for card's volts\n");
		return 0;
	}

	if (host->caps2 & MMC_CAP2_FULL_PWR_CYCLE) {
		bit = ffs(ocr) - 1;
		ocr &= 3 << bit;
		/* Power cycle */
		err = sd_uhs2_power_off(host);
		if (err)
			return 0;
		err = sd_uhs2_reinit(host);
		if (err)
			return 0;
	} else {
		bit = fls(ocr) - 1;
		ocr &= 3 << bit;
		if (bit != host->ios.vdd)
			dev_warn(mmc_dev(host), "exceeding card's volts\n");
	}

	return ocr;
}

/*
 * Initialize the UHS-II card through the SD-TRAN transport layer. This enables
 * commands/requests to be backwards compatible through the legacy SD protocol.
 * UHS-II cards has a specific power limit specified for VDD1/VDD2, that should
 * be set through a legacy CMD6. Note that, the power limit that becomes set,
 * survives a soft reset through the GO_DORMANT_STATE command.
 */
static int sd_uhs2_legacy_init(struct mmc_host *host, struct mmc_card *card)
{
	int err;
	u32 cid[4];
	u32 ocr;
	u32 rocr;
	u8  status[64];
	int ro;

	/* Send CMD0 to reset SD card */
	err = __mmc_go_idle(host);
	if (err)
		return err;

	mmc_delay(1);

	/* Send CMD8 to communicate SD interface operation condition */
	err = mmc_send_if_cond(host, host->ocr_avail);
	if (err) {
		dev_warn(mmc_dev(host), "CMD8 error\n");
		return err;
	}

	/*
	 * Probe SD card working voltage.
	 */
	err = mmc_send_app_op_cond(host, 0, &ocr);
	if (err)
		return err;

	card->ocr = ocr;

	/*
	 * Some SD cards claims an out of spec VDD voltage range. Let's treat
	 * these bits as being in-valid and especially also bit7.
	 */
	ocr &= ~0x7FFF;
	rocr = sd_uhs2_select_voltage(host, ocr);
	/*
	 * Some cards have zero value of rocr in UHS-II mode. Assign host's
	 * ocr value to rocr.
	 */
	if (!rocr)
		rocr = host->ocr_avail;

	rocr |= (SD_OCR_CCS | SD_OCR_XPC);

	/* Wait SD power on ready */
	ocr = rocr;

	err = mmc_send_app_op_cond(host, ocr, &rocr);
	if (err)
		return err;

	err = mmc_send_cid(host, cid);
	if (err)
		return err;

	memcpy(card->raw_cid, cid, sizeof(card->raw_cid));
	mmc_decode_cid(card);

	/*
	 * For native busses:  get card RCA and quit open drain mode.
	 */
	err = mmc_send_relative_addr(host, &card->rca);
	if (err)
		return err;

	err = mmc_sd_get_csd(card);
	if (err)
		return err;

	/*
	 * Select card, as all following commands rely on that.
	 */
	err = mmc_select_card(card);
	if (err)
		return err;

	/*
	 * Fetch SCR from card.
	 */
	err = mmc_app_send_scr(card);
	if (err)
		return err;

	err = mmc_decode_scr(card);
	if (err)
		return err;

	/*
	 * Switch to high power consumption mode.
	 * Even switch failed, sd card can still work at lower power consumption mode, but
	 * performance will be lower than high power consumption mode.
	 */
	if (!(card->csd.cmdclass & CCC_SWITCH)) {
		pr_warn("%s: card lacks mandatory switch function, performance might suffer\n",
			mmc_hostname(card->host));
	} else {
		/* send CMD6 to set Maximum Power Consumption to get better performance */
		err = mmc_sd_switch(card, 0, 3, SD4_SET_POWER_LIMIT_1_80W, status);
		if (!err)
			err = mmc_sd_switch(card, 1, 3, SD4_SET_POWER_LIMIT_1_80W, status);

		err = 0;
	}

	/*
	 * Check if read-only switch is active.
	 */
	ro = sd_uhs2_get_ro(host);
	if (ro < 0) {
		pr_warn("%s: host does not support read-only switch, assuming write-enable\n",
			mmc_hostname(host));
	} else if (ro > 0) {
		mmc_card_set_readonly(card);
	}

	/*
	 * NOTE:
	 * Should we read Externsion Register to check power notification feature here?
	 */

	return 0;
}

static void sd_uhs2_remove(struct mmc_host *host)
{
	mmc_remove_card(host->card);
	host->card = NULL;
	if (host->flags & MMC_UHS2_INITIALIZED)
		host->flags &= ~MMC_UHS2_INITIALIZED;
}

/*
 * Allocate the data structure for the mmc_card and run the UHS-II specific
 * initialization sequence.
 */
static int sd_uhs2_init_card(struct mmc_host *host, struct mmc_card *oldcard)
{
	struct mmc_card *card;
	u32 node_id;
	int err;

	err = sd_uhs2_dev_init(host);
	if (err)
		return err;

	err = sd_uhs2_enum(host, &node_id);
	if (err)
		return err;

	if (oldcard) {
		card = oldcard;
	} else {
		card = mmc_alloc_card(host, &sd_type);
		if (IS_ERR(card))
			return PTR_ERR(card);
	}
	host->card = card;

	card->uhs2_config.node_id = node_id;
	card->type = MMC_TYPE_SD;

	err = sd_uhs2_config_read(host, card);
	if (err)
		goto err;

	err = sd_uhs2_config_write(host, card);
	if (err)
		goto err;

	/* Change to Speed Range B if it is supported */
	if (card->uhs2_state & MMC_UHS2_SPEED_B) {
		err = sd_uhs2_change_speed(host, node_id);
		if (err)
			return err;
	}

	host->card->uhs2_state |= MMC_UHS2_INITIALIZED;
	host->flags |= MMC_UHS2_INITIALIZED;

	err = sd_uhs2_legacy_init(host, card);
	if (err)
		goto err;

	return 0;

err:
	if (host->card->uhs2_state & MMC_UHS2_INITIALIZED)
		host->card->uhs2_state &= ~MMC_UHS2_INITIALIZED;
	if (host->flags & MMC_UHS2_INITIALIZED)
		host->flags &= ~MMC_UHS2_INITIALIZED;
	sd_uhs2_remove(host);
	return err;
}

int sd_uhs2_reinit(struct mmc_host *host)
{
	struct mmc_card *card = host->card;
	int err;

	sd_uhs2_power_up(host);
	err = sd_uhs2_phy_init(host);
	if (err)
		return err;

	err = sd_uhs2_init_card(host, card);
	if (err)
		return err;

	mmc_card_set_present(card);
	return err;
}

static int sd_uhs2_alive(struct mmc_host *host)
{
	return mmc_send_status(host->card, NULL);
}

static void sd_uhs2_detect(struct mmc_host *host)
{
	int err;

	mmc_get_card(host->card, NULL);
	err = _mmc_detect_card_removed(host);
	mmc_put_card(host->card, NULL);

	if (err) {
		sd_uhs2_remove(host);

		mmc_claim_host(host);
		mmc_detach_bus(host);
		sd_uhs2_power_off(host);
		mmc_release_host(host);
	}
}

static int _sd_uhs2_suspend(struct mmc_host *host)
{
	struct mmc_card *card = host->card;
	int err = 0;

	mmc_claim_host(host);

	if (mmc_card_suspended(card))
		goto out;

	if (mmc_card_can_poweroff_notify(card))
		err = sd_poweroff_notify(card);

	if (!err) {
		sd_uhs2_power_off(host);
		mmc_card_set_suspended(card);
	}

out:
	mmc_release_host(host);
	return err;
}

/*
 * Callback for suspend
 */
static int sd_uhs2_suspend(struct mmc_host *host)
{
	int err;

	err = _sd_uhs2_suspend(host);
	if (!err) {
		pm_runtime_disable(&host->card->dev);
		pm_runtime_set_suspended(&host->card->dev);
	}

	return err;
}

/*
 * This function tries to determine if the same card is still present
 * and, if so, restore all state to it.
 */
static int _mmc_sd_uhs2_resume(struct mmc_host *host)
{
	int err = 0;

	mmc_claim_host(host);

	if (!mmc_card_suspended(host->card))
		goto out;

	/* Power up UHS2 SD card and re-initialize it. */
	err = sd_uhs2_reinit(host);
	mmc_card_clr_suspended(host->card);

out:
	mmc_release_host(host);
	return err;
}

/*
 * Callback for resume
 */
static int sd_uhs2_resume(struct mmc_host *host)
{
	pm_runtime_enable(&host->card->dev);
	return 0;
}

/*
 * Callback for runtime_suspend.
 */
static int sd_uhs2_runtime_suspend(struct mmc_host *host)
{
	int err;

	if (!(host->caps & MMC_CAP_AGGRESSIVE_PM))
		return 0;

	err = _sd_uhs2_suspend(host);
	if (err)
		pr_err("%s: error %d doing aggressive suspend\n", mmc_hostname(host), err);

	return err;
}

static int sd_uhs2_runtime_resume(struct mmc_host *host)
{
	int err;

	err = _mmc_sd_uhs2_resume(host);
	if (err && err != -ENOMEDIUM)
		pr_err("%s: error %d doing runtime resume\n", mmc_hostname(host), err);

	return err;
}

static int sd_uhs2_hw_reset(struct mmc_host *host)
{
	int err;

	sd_uhs2_power_off(host);
	/* Wait at least 1 ms according to SD spec */
	mmc_delay(1);
	sd_uhs2_power_up(host);

	err = sd_uhs2_reinit(host);

	return err;
}

/*
 * mmc_uhs2_prepare_cmd - prepare for SD command packet
 * @host:	MMC host
 * @mrq:	MMC request
 *
 * Initialize and fill in a header and a payload of SD command packet.
 * The caller should allocate uhs2_command in host->cmd->uhs2_cmd in
 * advance.
 *
 * Return:	0 on success, non-zero error on failure
 */
void mmc_uhs2_prepare_cmd(struct mmc_host *host, struct mmc_request *mrq)
{
	struct mmc_command *cmd;
	struct uhs2_command *uhs2_cmd;
	u16 header, arg;
	__be32 *payload;
	u8 plen;

	cmd = mrq->cmd;
	header = host->card->uhs2_config.node_id;
	if ((cmd->flags & MMC_CMD_MASK) == MMC_CMD_ADTC)
		header |= UHS2_PACKET_TYPE_DCMD;
	else
		header |= UHS2_PACKET_TYPE_CCMD;

	arg = cmd->opcode << UHS2_SD_CMD_INDEX_POS;
	if (host->uhs2_ios.is_APP_CMD) {
		arg |= UHS2_SD_CMD_APP;
		host->uhs2_ios.is_APP_CMD = false;
	}

	uhs2_cmd = cmd->uhs2_cmd;
	payload = uhs2_cmd->payload;
	plen = 2; /* at the maximum */

	if ((cmd->flags & MMC_CMD_MASK) == MMC_CMD_ADTC &&
	    !cmd->uhs2_tmode0_flag) {
		if (host->uhs2_ios.is_2L_HD_mode)
			arg |= UHS2_DCMD_2L_HD_MODE;

		arg |= UHS2_DCMD_LM_TLEN_EXIST;

		if (cmd->data->blocks == 1 &&
		    cmd->data->blksz != 512 &&
		    cmd->opcode != MMC_READ_SINGLE_BLOCK &&
		    cmd->opcode != MMC_WRITE_BLOCK) {
			arg |= UHS2_DCMD_TLUM_BYTE_MODE;
			payload[1] = cpu_to_be32(cmd->data->blksz);
		} else {
			payload[1] = cpu_to_be32(cmd->data->blocks);
		}
	} else {
		plen = 1;
	}

	payload[0] = cpu_to_be32(cmd->arg);
	sd_uhs2_cmd_assemble(cmd, uhs2_cmd, header, arg, payload, plen, NULL, 0);
}

static const struct mmc_bus_ops sd_uhs2_ops = {
	.remove = sd_uhs2_remove,
	.alive = sd_uhs2_alive,
	.detect = sd_uhs2_detect,
	.suspend = sd_uhs2_suspend,
	.resume = sd_uhs2_resume,
	.runtime_suspend = sd_uhs2_runtime_suspend,
	.runtime_resume = sd_uhs2_runtime_resume,
	.shutdown = sd_uhs2_suspend,
	.hw_reset = sd_uhs2_hw_reset,
};

static int sd_uhs2_attach(struct mmc_host *host)
{
	int err;

	host->flags |= MMC_UHS2_SUPPORT;

	err = sd_uhs2_power_up(host);
	if (err)
		goto err;

	err = sd_uhs2_phy_init(host);
	if (err)
		goto err;

	err = sd_uhs2_init_card(host, NULL);
	if (err)
		goto err;

	mmc_attach_bus(host, &sd_uhs2_ops);

	mmc_release_host(host);

	err = mmc_add_card(host->card);
	if (err)
		goto remove_card;

	mmc_claim_host(host);

	host->ops->uhs2_control(host, UHS2_POST_ATTACH_SD);

	return 0;

remove_card:
	sd_uhs2_remove(host);
	mmc_claim_host(host);

err:
	mmc_detach_bus(host);
	sd_uhs2_power_off(host);
	host->flags &= ~MMC_UHS2_SUPPORT;
	return 1;
}

/**
 * mmc_attach_sd_uhs2 - select UHS2 interface
 * @host: MMC host
 *
 * Try to select UHS2 interface and initialize the bus for a given
 * frequency, @freq.
 *
 * Return:	0 on success, non-zero error on failure
 */
int mmc_attach_sd_uhs2(struct mmc_host *host)
{
	int i, err;

	if (!(host->caps2 & MMC_CAP2_SD_UHS2))
		return -EOPNOTSUPP;

	/* Turn off the legacy SD interface before trying with UHS-II. */
	mmc_power_off(host);

	/*
	 * Start UHS-II initialization at 52MHz and possibly make a retry at
	 * 26MHz according to the spec. It's required that the host driver
	 * validates ios->clock, to set a rate within the correct range.
	 */
	for (i = 0; i < ARRAY_SIZE(sd_uhs2_freqs); i++) {
		host->f_init = sd_uhs2_freqs[i];
		pr_info("%s: %s: trying to init UHS-II card at %u Hz\n",
			mmc_hostname(host), __func__, host->f_init);

		err = sd_uhs2_attach(host);
		if (!err)
			break;
	}

	return err;
}
