// SPDX-License-Identifier: GPL-2.0
/*
 * Transport for THC ports in IPTS mode: plain SPI with a private framing.
 */

#include <linux/delay.h>
#include <linux/math.h>
#include <linux/regmap.h>
#include <linux/string.h>

#include "intel-thc-hw.h"
#include "ipts.h"

/* Bulk transfers have no address phase, so the SPI opcode is zero. */
#define THC_IPTS_SPI_OPCODE		0

/* Slowest clock the core can program, used until the controller is read. */
#define THC_IPTS_BOOT_SPI_HZ		THC_IPTS_SPI_MIN_HZ

/* Time the controller needs after reset before it answers on the bus. */
#define THC_IPTS_BOOT_MS		100

static int thc_ipts_read_cfg(struct thc_ipts *idev)
{
	int ret, try;

	for (try = 0; try < THC_IPTS_CFG_TRIES; try++) {
		u32 got = 0;

		memset(&idev->cfg, 0, sizeof(idev->cfg));
		ret = thc_tic_pio_read(idev->thc, 0, THC_IPTS_CFG_SIZE, &got,
				       (u32 *)&idev->cfg);
		if (!ret && got == THC_IPTS_CFG_SIZE &&
		    le32_to_cpu(idev->cfg.device_id) == THC_IPTS_TIC_ID)
			return 0;

		if (try + 1 < THC_IPTS_CFG_TRIES)
			msleep(THC_IPTS_CFG_RETRY_MS);
	}

	dev_err(idev->dev,
		"controller did not answer, last id 0x%08x, error %d\n",
		le32_to_cpu(idev->cfg.device_id), ret ?: -EIO);

	return ret ?: -EIO;
}

/* Reading first keeps the core from warning when the state already matches. */
static int thc_ipts_quiesce(struct thc_ipts *idev, bool quiesce)
{
	u32 ctrl;
	int ret;

	ret = regmap_read(idev->thc->thc_regmap, THC_M_PRT_CONTROL_OFFSET,
			  &ctrl);
	if (ret)
		return ret;

	if (!!(ctrl & THC_M_PRT_CONTROL_THC_DEVINT_QUIESCE_EN) == quiesce)
		return 0;

	return thc_interrupt_quiesce(idev->thc, quiesce);
}

static int thc_ipts_spi_set(struct thc_ipts *idev, u32 hz, u32 mode)
{
	int ret;

	/* No multi-packet transfers in this mode, so both sizes stay zero. */
	ret = thc_spi_read_config(idev->thc, hz, mode, THC_IPTS_SPI_OPCODE, 0);
	if (ret)
		return ret;

	return thc_spi_write_config(idev->thc, hz, mode, THC_IPTS_SPI_OPCODE,
				     0, 0);
}

/* The core takes the fastest gear that is not above the request. */
static void thc_ipts_spi_from_cfg(struct thc_ipts *idev, u32 *hz, u32 *mode)
{
	u32 cfg = le32_to_cpu(idev->cfg.spi_cfg);
	u32 div = THC_IPTS_SPI_CLKDIV(cfg);

	if (!div) {
		dev_warn_once(idev->dev, "controller asks for divisor 0\n");
		*hz = THC_IPTS_SPI_MIN_HZ;
	} else {
		u32 step = THC_IPTS_SPI_BASE_HZ / div;

		if (cfg & THC_IPTS_SPI_CLKDIV8)
			step /= THC_IPTS_SPI_CLKDIV8_DIV;

		/* Gears compare rounded to 100 Hz: round the same way, or
		 * divisor 3 falls below the 41 MHz gear.
		 */
		*hz = DIV_ROUND_CLOSEST(step, 100) * 100;
	}

	/*
	 * THC_IPTS_SPI_MIN_HZ is the slowest gear the core has, so a lower
	 * request would be refused: run at the floor and keep the port.
	 */
	if (*hz < THC_IPTS_SPI_MIN_HZ) {
		dev_warn_once(idev->dev,
			      "controller asks for %u Hz, running at %u Hz\n",
			      *hz, THC_IPTS_SPI_MIN_HZ);
		*hz = THC_IPTS_SPI_MIN_HZ;
	}

	if (cfg & THC_IPTS_SPI_QUAD)
		*mode = THC_QUAD_IO;
	else if (cfg & THC_IPTS_SPI_DUAL)
		*mode = THC_DUAL_IO;
	else
		*mode = THC_SINGLE_IO;
}

static int thc_ipts_touch_set(struct thc_ipts *idev, bool enable)
{
	u32 touch = le32_to_cpu(idev->cfg.touch_cfg);

	touch &= ~(THC_IPTS_TOUCH_POWER_MASK | THC_IPTS_TOUCH_ENABLE |
		   THC_IPTS_TOUCH_HID_REPORT);
	touch |= THC_IPTS_TOUCH_POWER(enable ? THC_IPTS_TOUCH_POWER_ON : 0);
	if (enable)
		touch |= THC_IPTS_TOUCH_ENABLE | THC_IPTS_TOUCH_HID_REPORT;

	return thc_tic_pio_write(idev->thc, THC_IPTS_CFG_TOUCH, sizeof(touch),
				 &touch);
}

/**
 * thc_ipts_port_start - bring one port up
 * @idev: port to start
 */
int thc_ipts_port_start(struct thc_ipts *idev)
{
	u32 ctrl, hz, mode;
	int ret;

	ret = thc_ipts_quiesce(idev, true);
	if (ret)
		return ret;

	ret = thc_port_select(idev->thc, THC_PORT_TYPE_SPI);
	if (ret)
		return ret;

	ret = regmap_read(idev->thc->thc_regmap, THC_M_PRT_CONTROL_OFFSET,
			  &ctrl);
	if (ret)
		return ret;
	if (!(ctrl & THC_M_PRT_CONTROL_PORT_SUPPORTED))
		return -ENODEV;

	/* Without this the engines write into the configuration block. */
	thc_spi_input_output_address_config(idev->thc, 0, THC_IPTS_BULK_ADDR,
					    THC_IPTS_BULK_ADDR);

	/*
	 * Firmware may leave the receive engines armed with the PRD base of
	 * whatever it had mapped. Stop them before the controller starts
	 * pushing frames again, or those frames land in stale pages.
	 */
	thc_dma_unconfigure(idev->thc);

	ret = thc_ipts_spi_set(idev, THC_IPTS_BOOT_SPI_HZ, THC_SINGLE_IO);
	if (ret)
		return ret;

	/* The first configuration read is unreliable without edge detect. */
	thc_int_trigger_type_select(idev->thc, true);

	/*
	 * Release the controller from reset; it answers only after this.
	 * The bit is the reset request and one means out of reset, so it has
	 * to be written whatever it reads back.
	 */
	ret = regmap_write_bits(idev->thc->thc_regmap, THC_M_PRT_CONTROL_OFFSET,
				THC_M_PRT_CONTROL_DEVRST,
				THC_M_PRT_CONTROL_DEVRST);
	if (ret)
		return ret;

	msleep(THC_IPTS_BOOT_MS);

	ret = thc_ipts_read_cfg(idev);
	if (ret)
		return ret;

	idev->vendor = le16_to_cpu(idev->cfg.vendor);
	idev->product = le16_to_cpu(idev->cfg.product);
	idev->max_rx = THC_IPTS_RX_BUF_SIZE(le32_to_cpu(idev->cfg.buf_sizes));
	idev->max_tx = THC_IPTS_TX_BUF_SIZE(le32_to_cpu(idev->cfg.buf_sizes));
	/* TX_BUF_SIZE() never goes below 64, so only the rx half can fail. */
	if (idev->max_rx <= THC_IPTS_RX_HDR_SIZE) {
		dev_err(idev->dev, "receive buffer of %zu bytes is too small\n",
			idev->max_rx);
		return -ENODEV;
	}
	thc_ipts_spi_from_cfg(idev, &hz, &mode);
	ret = thc_ipts_spi_set(idev, hz, mode);
	if (ret) {
		dev_warn(idev->dev,
			 "cannot follow the SPI timing, going slow\n");
		ret = thc_ipts_spi_set(idev, THC_IPTS_BOOT_SPI_HZ,
				       THC_SINGLE_IO);
		if (ret)
			return ret;
	} else {
		dev_dbg(idev->dev, "SPI clock asked for %u Hz\n", hz);
	}

	ret = thc_ipts_quiesce(idev, false);
	if (ret)
		return ret;

	ret = thc_ipts_touch_set(idev, true);
	if (ret)
		return ret;
	idev->running = true;

	return 0;
}

/**
 * thc_ipts_port_stop - tear one port down
 * @idev: port to stop
 */
void thc_ipts_port_stop(struct thc_ipts *idev)
{
	if (idev->running)
		thc_ipts_touch_set(idev, false);

	idev->running = false;
	/* Forced: the bit may read as clear and still need the command. */
	regmap_write_bits(idev->thc->thc_regmap, THC_M_PRT_CONTROL_OFFSET,
			  THC_M_PRT_CONTROL_DEVRST, 0);
	thc_ipts_quiesce(idev, true);
}
