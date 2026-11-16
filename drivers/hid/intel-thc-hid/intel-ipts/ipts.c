// SPDX-License-Identifier: GPL-2.0
/*
 * Transport for THC ports in IPTS mode: plain SPI with a private framing.
 */

#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/minmax.h>
#include <linux/sizes.h>
#include <linux/string.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>

#include "intel-thc-hw.h"
#include "ipts.h"

/* Bulk transfers have no address phase, so the SPI opcode is zero. */
#define THC_IPTS_SPI_OPCODE		0

/* Slowest clock the core can program, used until the controller is read. */
#define THC_IPTS_BOOT_SPI_HZ		THC_IPTS_SPI_MIN_HZ

/* One page per PRD entry; a larger frame cannot be received. */
#define THC_IPTS_RX_MAX			(PRD_ENTRIES_NUM * SZ_4K)

/* Time the controller needs after reset before it answers on the bus. */
#define THC_IPTS_BOOT_MS		100

/*
 * The core stops at the first error cause and reports the rest as a mask, so
 * reading until it finds nothing can take a pass per cause plus one that
 * confirms the status is drained.
 */
#define THC_IPTS_IRQ_PASSES		(THC_UNKNOWN_INT + 1)

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

	*hz = thc_spi_hz_from_div(THC_IPTS_SPI_CLKDIV(cfg),
				  cfg & THC_IPTS_SPI_CLKDIV8);
	if (!*hz) {
		dev_warn_once(idev->dev, "controller asks for divisor 0\n");
		*hz = THC_IPTS_SPI_MIN_HZ;
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

static void thc_ipts_handle_frame(struct thc_ipts *idev, size_t len)
{
	size_t capacity = idev->max_rx - THC_IPTS_RX_HDR_SIZE;
	u32 code, size;

	if (len < THC_IPTS_RX_HDR_SIZE)
		return;

	code = get_unaligned_le32(idev->rx_frame);
	/* The header may claim more than what actually arrived. */
	size = min_t(u32, get_unaligned_le32(idev->rx_frame + 4),
		     capacity);
	size = min_t(u32, size, len - THC_IPTS_RX_HDR_SIZE);

	dev_dbg(idev->dev, "frame code %u, %u bytes\n", code, size);
}

static void thc_ipts_dma_stop(struct thc_ipts *idev)
{
	if (READ_ONCE(idev->state) < THC_IPTS_DMA)
		return;

	WRITE_ONCE(idev->state, THC_IPTS_CONFIG);
	thc_dma_unconfigure(idev->thc);
	thc_dma_release(idev->thc);
}

static int thc_ipts_resize(struct thc_ipts *idev, u8 **buf, size_t *have,
			   size_t want)
{
	u8 *p;

	if (*have >= want)
		return 0;

	p = devm_krealloc(idev->dev, *buf, want, GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	*buf = p;
	*have = want;

	return 0;
}

static int thc_ipts_dma_start(struct thc_ipts *idev)
{
	int ret;

	/*
	 * The receive ring has one page per PRD entry and the core refuses a
	 * frame that needs more: bound it here, or the port fails with a bare
	 * allocation error from sgl_alloc().
	 */
	if (idev->max_rx > THC_IPTS_RX_MAX) {
		dev_err(idev->dev, "receive buffer of %zu bytes is too large\n",
			idev->max_rx);
		return -ENODEV;
	}

	/*
	 * A resume can report larger limits than the probe did, and the
	 * engines would then write past the end. Follow them on every start;
	 * the receive buffer is rounded up because the core rounds to a page.
	 */
	ret = thc_ipts_resize(idev, &idev->rx_frame, &idev->buf_rx,
			      round_up(idev->max_rx, SZ_4K));
	if (ret)
		return ret;

	ret = thc_dma_set_max_packet_sizes(idev->thc, 0, idev->max_rx,
					   idev->max_tx, 0);
	if (ret)
		return ret;

	ret = thc_dma_allocate(idev->thc);
	if (ret)
		return ret;

	ret = thc_dma_configure(idev->thc);
	if (ret) {
		thc_dma_release(idev->thc);
		return ret;
	}

	return 0;
}

irqreturn_t thc_ipts_hardirq(int irq, void *data)
{
	struct thc_ipts *idev = data;

	/* Nothing of ours is pending: the cause belongs to a sharer. A reset
	 * in flight owns the status registers and acknowledges nothing.
	 */
	if (READ_ONCE(idev->state) < THC_IPTS_DMA ||
	    READ_ONCE(idev->recovering))
		return IRQ_NONE;

	/* Level triggered and shared: mask first, or it stays asserted. */
	thc_interrupt_enable(idev->thc, false);

	/*
	 * A teardown in flight still gets the thread: the status it drains is
	 * where a pending transmit completion is delivered, and a write
	 * waiting for that one holds the bus lock the teardown needs. A reset
	 * needs no such help, it masks for itself before it waits.
	 */
	return IRQ_WAKE_THREAD;
}

static int thc_ipts_read_frames(struct thc_ipts *idev)
{
	for (;;) {
		int finished = 0;
		size_t len = 0;

		if (thc_rxdma_read(idev->thc, THC_RXDMA2, idev->rx_frame,
				   &len, &finished)) {
			dev_warn_once(idev->dev,
				      "cannot read a frame from the engine\n");
			return -EIO;
		}

		if (!len)
			break;

		thc_ipts_handle_frame(idev, len);
		if (finished)
			break;
	}

	return 0;
}

/*
 * Refuse new requests and queue the reset. The line stays open: a write that
 * is already waiting for its completion needs this interrupt, and the work
 * item takes the port once that write is done.
 */
static void thc_ipts_request_recover(struct thc_ipts *idev)
{
	if (READ_ONCE(idev->recover_pending))
		return;

	WRITE_ONCE(idev->recover_pending, true);
	schedule_work(&idev->recover_work);
}

/*
 * From a work item: the reset waits for the bus lock a pending write holds
 * until the threaded handler delivers its completion.
 */
void thc_ipts_recover(struct work_struct *work)
{
	struct thc_ipts *idev = container_of(work, struct thc_ipts,
					     recover_work);

	/*
	 * From here the status registers belong to the reset: the threaded
	 * handler must not acknowledge a frame it is not going to read, or the
	 * interrupt that would announce it again never comes. The flag alone
	 * is only a snapshot to a run that is already inside, so the mask and
	 * the wait below are what keeps that run out.
	 */
	WRITE_ONCE(idev->recovering, true);
	thc_interrupt_enable(idev->thc, false);
	synchronize_irq(idev->irq);
	/* A run that unmasked just before synchronize_irq() returned leaves
	 * the line open: mask it again, the write is idempotent.
	 */
	thc_interrupt_enable(idev->thc, false);

	if (thc_rxdma_reset(idev->thc)) {
		/* Nothing left: keep the port masked until it restarts. */
		dev_err(idev->dev, "cannot reset the receive engine\n");
		return;
	}

	/* The reset drops whatever the engines had queued. A run of the
	 * handler that sees ->recovering clear must not be turned away by
	 * ->recover_pending afterwards, so that one is cleared first.
	 */
	WRITE_ONCE(idev->recover_seq, READ_ONCE(idev->recover_seq) + 1);
	WRITE_ONCE(idev->recover_pending, false);
	WRITE_ONCE(idev->recovering, false);
	if (!READ_ONCE(idev->stopping))
		thc_interrupt_enable(idev->thc, true);
}

irqreturn_t thc_ipts_thread(int irq, void *data)
{
	struct thc_ipts *idev = data;
	irqreturn_t ret = IRQ_NONE;
	bool need_recover = false;
	bool reported = false;
	int pass;

	/* Reports are handed to the HID core from here, and a driver callback
	 * that sends a request would wait for a completion this very thread
	 * delivers: it is recorded so the send path can tell.
	 */
	WRITE_ONCE(idev->irq_thread, current);

	/* A sharer can queue a second run of this thread with no hardirq of
	 * ours in between: mask again, the write is idempotent.
	 */
	thc_interrupt_enable(idev->thc, false);

	/* The port stays masked, so a cause that arrives meanwhile leaves only
	 * its status bit: read until the core reports none.
	 */
	for (pass = 0; pass < THC_IPTS_IRQ_PASSES; pass++) {
		int mask;

		/*
		 * The reset owns the status registers while it runs: a frame
		 * acknowledged here but not read is never announced again.
		 * Nothing is acknowledged here either, so the line is not
		 * claimed.
		 */
		if (READ_ONCE(idev->recovering))
			break;

		mask = thc_interrupt_handler(idev->thc);

		if (mask == BIT(THC_UNKNOWN_INT))
			break;

		ret = IRQ_HANDLED;
		if (mask & (BIT(THC_FATAL_ERR_INT) | BIT(THC_TXN_ERR_INT))) {
			/* An overrun raises this for every frame until the
			 * reset takes the port: report the first one only.
			 */
			if (!reported) {
				dev_err(idev->dev,
					"transfer error, resetting the engine\n");
				reported = true;
			}
			/* The teardown has its own reset to wait for. */
			if (!READ_ONCE(idev->stopping))
				need_recover = true;
			/* The core stops at the first cause: a Tx completion
			 * behind the error would time out under the bus lock.
			 */
			continue;
		}

		/* A reset is queued or running, or the port is going away: the
		 * ring is not ours to read, only its status was.
		 */
		if (need_recover || READ_ONCE(idev->recover_pending) ||
		    READ_ONCE(idev->recovering) ||
		    READ_ONCE(idev->stopping))
			continue;

		if (mask & BIT(THC_RXDMA2_INT) && thc_ipts_read_frames(idev))
			need_recover = true;
	}

	/* The work item resets the engine and unmasks again; a frame read
	 * meanwhile would corrupt the ring.
	 */
	if (need_recover) {
		thc_ipts_request_recover(idev);
		return IRQ_HANDLED;
	}

	/* A reset unmasks when it is done and a teardown masks on its own, so
	 * the line only stays open here while neither runs: a write already
	 * on the wire needs the completion this handler delivers.
	 */
	if (!READ_ONCE(idev->recovering) && !READ_ONCE(idev->stopping))
		thc_interrupt_enable(idev->thc, true);

	return ret;
}

/**
 * thc_ipts_port_start - bring one port up
 * @idev: port to start
 */
int thc_ipts_port_start(struct thc_ipts *idev)
{
	u32 hz, mode;
	bool supported;
	int ret;

	WRITE_ONCE(idev->stopping, false);
	WRITE_ONCE(idev->recovering, false);

	ret = thc_interrupt_quiesce(idev->thc, true);
	if (ret)
		return ret;

	ret = thc_port_select(idev->thc, THC_PORT_TYPE_SPI);
	if (ret)
		return ret;

	ret = thc_port_supported(idev->thc, &supported);
	if (ret)
		return ret;
	if (!supported)
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
	ret = thc_port_reset_release(idev->thc);
	if (ret)
		return ret;

	/* Out of reset from here: a teardown has to put it back. */
	WRITE_ONCE(idev->state, THC_IPTS_RESET);

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

	ret = thc_interrupt_quiesce(idev->thc, false);
	if (ret)
		return ret;

	ret = thc_ipts_touch_set(idev, true);
	if (ret)
		return ret;
	WRITE_ONCE(idev->state, THC_IPTS_CONFIG);

	/* Sets the type field to invalid; RXDMA2 start writes the real one. */
	thc_interrupt_config(idev->thc);

	ret = thc_ipts_dma_start(idev);
	if (ret)
		return ret;
	WRITE_ONCE(idev->state, THC_IPTS_DMA);

	thc_interrupt_enable(idev->thc, true);

	return 0;
}

/**
 * thc_ipts_port_stop - tear one port down
 * @idev: port to stop
 */
void thc_ipts_port_stop(struct thc_ipts *idev)
{
	/*
	 * Mask the line, then wait for a handler that may be holding a
	 * frame for the HID device which is about to go away.
	 */
	WRITE_ONCE(idev->stopping, true);
	thc_interrupt_enable(idev->thc, false);
	synchronize_irq(idev->irq);
	cancel_work_sync(&idev->recover_work);

	/* A handler that read ->stopping early may have unmasked. */
	thc_interrupt_enable(idev->thc, false);

	/* Stop the controller before the buffers it writes into go away. */
	if (READ_ONCE(idev->state) >= THC_IPTS_CONFIG)
		thc_ipts_touch_set(idev, false);

	/* Forced: the bit may read as clear and still need the command. */
	thc_port_reset_hold(idev->thc);
	thc_interrupt_quiesce(idev->thc, true);

	thc_ipts_dma_stop(idev);
	WRITE_ONCE(idev->state, THC_IPTS_DOWN);
}
