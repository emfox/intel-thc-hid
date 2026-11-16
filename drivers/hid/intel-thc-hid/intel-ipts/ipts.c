// SPDX-License-Identifier: GPL-2.0
/*
 * Transport for THC ports in IPTS mode: plain SPI with a private framing.
 */

#include <linux/cleanup.h>
#include <linux/delay.h>
#include <linux/math.h>
#include <linux/hid.h>
#include <linux/interrupt.h>
#include <linux/minmax.h>
#include <linux/regmap.h>
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

/*
 * Frames are built in @idev->tx_frame, so the lock is taken here rather than
 * by the callers: none of them may hold it across a wait.
 */
static int thc_ipts_send(struct thc_ipts *idev, u32 code, const void *data,
			 size_t len)
{
	size_t padded = round_up(len, 4);
	u8 *frame = idev->tx_frame;

	/* The completion of a write is delivered by the threaded handler, so
	 * one issued from there would wait out its timeout: refuse it instead,
	 * and let the caller retry from its own context.
	 */
	if (READ_ONCE(idev->in_thread))
		return -EBUSY;

	guard(mutex)(&idev->tx_lock);

	if (READ_ONCE(idev->stopping))
		return -ESHUTDOWN;
	/* A reset is queued or running: the write would only time out. */
	if (READ_ONCE(idev->recover_pending) || READ_ONCE(idev->recovering))
		return -EAGAIN;
	/* No engine to write into: waiting for a reset would not help. */
	if (!READ_ONCE(idev->dma_ready))
		return -ENODEV;
	if (THC_IPTS_TX_HDR_SIZE + padded > idev->max_tx)
		return -EMSGSIZE;

	put_unaligned_le32(code, frame);
	put_unaligned_le32((u32)len, frame + 4);
	memset(frame + THC_IPTS_TX_HDR_SIZE, 0, padded);
	if (len && data)
		memcpy(frame + THC_IPTS_TX_HDR_SIZE, data, len);

	return thc_dma_write(idev->thc, frame, THC_IPTS_TX_HDR_SIZE + padded);
}

static void thc_ipts_descriptor(struct thc_ipts *idev, const u8 *data,
				size_t len)
{
	/* One answer per request: a late reply must not overwrite it. */
	if (len <= THC_IPTS_DESC_PREAMBLE || READ_ONCE(idev->have_desc))
		return;

	len = min(len - THC_IPTS_DESC_PREAMBLE, idev->max_rx);
	memcpy(idev->desc, data + THC_IPTS_DESC_PREAMBLE, len);
	idev->desc_len = len;
	/* The bytes and the length have to land before the flag is seen. */
	smp_store_release(&idev->have_desc, true);
	wake_up(&idev->wq);
}

static void thc_ipts_feature(struct thc_ipts *idev, const u8 *data,
			     size_t len)
{
	if (!len)
		return;

	/* Only a frame that arrives while a request is waiting is taken. */
	mutex_lock(&idev->feature_lock);
	if (!idev->feature_pending ||
	    (idev->feature_id && data[0] != idev->feature_id)) {
		mutex_unlock(&idev->feature_lock);
		return;
	}

	len = min(len, idev->max_rx);
	memcpy(idev->feature, data, len);
	idev->feature_len = len;
	WRITE_ONCE(idev->feature_done, true);
	mutex_unlock(&idev->feature_lock);

	wake_up(&idev->wq);
}

static void thc_ipts_handle_frame(struct thc_ipts *idev, size_t len)
{
	const u8 *payload = idev->rx_frame + THC_IPTS_RX_HDR_SIZE;
	size_t capacity = idev->max_rx - THC_IPTS_RX_HDR_SIZE;
	u32 code, size;

	if (len < THC_IPTS_RX_HDR_SIZE)
		return;

	code = get_unaligned_le32(idev->rx_frame);
	/* The header may claim more than what actually arrived. */
	size = min_t(u32, get_unaligned_le32(idev->rx_frame + 4), capacity);
	size = min_t(u32, size, len - THC_IPTS_RX_HDR_SIZE);

	switch (code) {
	case THC_IPTS_RX_INPUT: {
		struct hid_device *hdev = READ_ONCE(idev->hid);

		if (hdev)
			hid_safe_input_report(hdev, HID_INPUT_REPORT,
					      (u8 *)payload, capacity, size, 1);
		break;
	}
	case THC_IPTS_RX_DESC:
		thc_ipts_descriptor(idev, payload, size);
		break;
	case THC_IPTS_RX_FEATURE:
		thc_ipts_feature(idev, payload, size);
		break;
	case THC_IPTS_RX_RESET:
		dev_warn(idev->dev, "controller asked for a reset\n");
		break;
	default:
		dev_dbg(idev->dev, "frame code %u, %u bytes\n", code, size);
		break;
	}
}

static void thc_ipts_dma_stop(struct thc_ipts *idev)
{
	if (!idev->dma_ready)
		return;

	WRITE_ONCE(idev->dma_ready, false);
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
	if (!ret)
		ret = thc_ipts_resize(idev, &idev->tx_frame, &idev->buf_tx,
				      round_up(idev->max_tx, 4));
	if (!ret)
		ret = thc_ipts_resize(idev, &idev->desc, &idev->buf_desc,
				      idev->max_rx);
	if (!ret)
		ret = thc_ipts_resize(idev, &idev->feature, &idev->buf_feat,
				      idev->max_rx);
	if (ret)
		return ret;

	/* Only RXDMA2 (in) and TXDMA (out) are used; the other two stay 0. */
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

	WRITE_ONCE(idev->dma_ready, true);

	return 0;
}

irqreturn_t thc_ipts_hardirq(int irq, void *data)
{
	struct thc_ipts *idev = data;

	/* Nothing of ours is pending: the cause belongs to a sharer. */
	if (!READ_ONCE(idev->dma_ready))
		return IRQ_NONE;

	/* Level triggered and shared: mask first, or it stays asserted. */
	thc_interrupt_enable(idev->thc, false);

	/*
	 * A reset or a teardown in flight still gets the thread: the status it
	 * drains is where a pending transmit completion is delivered, and a
	 * write waiting for that one holds the bus lock the reset needs.
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
	/* A request waiting for the port looks at that flag. */
	wake_up(&idev->wq);
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
	 * A write that is already on the wire holds this lock and needs the
	 * interrupt that the mask below takes away: let it finish first, or it
	 * times out over the bus lock the reset is waiting for.
	 */
	mutex_lock(&idev->tx_lock);
	mutex_unlock(&idev->tx_lock);

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

	if (thc_rxdma_reset(idev->thc)) {
		/* Nothing left: keep the port masked until it restarts. */
		dev_err(idev->dev, "cannot reset the receive engine\n");
		/* It stays masked, so the waiters learn it from the flags. */
		wake_up(&idev->wq);
		return;
	}

	/* The reset drops whatever the engines had queued. A run of the
	 * handler that sees ->recovering clear must not be turned away by
	 * ->recover_pending afterwards, so that one is cleared first.
	 */
	WRITE_ONCE(idev->recover_seq, READ_ONCE(idev->recover_seq) + 1);
	WRITE_ONCE(idev->recover_pending, false);
	WRITE_ONCE(idev->recovering, false);

	/* A request waiting out the reset is looking at that flag. */
	wake_up(&idev->wq);

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
	 * delivers: thc_ipts_send() refuses those.
	 */
	WRITE_ONCE(idev->in_thread, true);

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
		 */
		if (READ_ONCE(idev->recovering)) {
			/* Nothing acknowledged, but the line is still ours. */
			ret = IRQ_HANDLED;
			break;
		}

		mask = thc_interrupt_handler(idev->thc);

		if (mask == BIT(THC_UNKNOWN_INT))
			break;

		ret = IRQ_HANDLED;
		if (mask & (BIT(THC_FATAL_ERR_INT) | BIT(THC_TXN_ERR_INT))) {
			/* An overrun raises this until the reset takes the
			 * port: report the first one only.
			 */
			if (!reported && !READ_ONCE(idev->recover_pending)) {
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

	/*
	 * The work item resets the engine in its own time; a frame read
	 * meanwhile would corrupt the ring, so only queue the reset.
	 */
	if (need_recover)
		thc_ipts_request_recover(idev);

	/*
	 * A reset unmasks when it is done, and a teardown masks on its own.
	 * Leaving the line open here is what a write already on the wire
	 * needs: this handler is where its completion is delivered, and
	 * masking it away would make that write time out over the bus lock
	 * the reset is waiting for.
	 */
	if (!READ_ONCE(idev->recovering))
		thc_interrupt_enable(idev->thc, true);

	WRITE_ONCE(idev->in_thread, false);

	return ret;
}

/* No reset is queued or running, and the port is not going away. */
static bool thc_ipts_idle(const struct thc_ipts *idev)
{
	return !READ_ONCE(idev->recover_pending) &&
	       !READ_ONCE(idev->recovering) &&
	       !READ_ONCE(idev->stopping);
}

static int thc_ipts_hid_parse(struct hid_device *hdev)
{
	struct thc_ipts *idev = hdev->driver_data;
	unsigned long timeout = msecs_to_jiffies(THC_IPTS_REQ_TIMEOUT_MS);
	u64 zero = 0;
	unsigned int retries = 0;
	int ret;
	int try = 0;

	/* A descriptor from a previous start is not this one's. */
	WRITE_ONCE(idev->have_desc, false);

	while (try < THC_IPTS_REQ_TRIES) {
		/* An answer that landed after a wait expired is taken here. */
		if (smp_load_acquire(&idev->have_desc))
			break;

		/* The controller ignores the eight byte argument. */
		ret = thc_ipts_send(idev, THC_IPTS_TX_GET_DESC, &zero,
				    sizeof(zero));
		/*
		 * -EAGAIN is our own reset refusing the request. -ETIMEDOUT is
		 * a write that was already on the wire when one started: its
		 * completion went away with the mask. Both are worth another
		 * try, a descriptor request carries nothing a late answer could
		 * spoil.
		 */
		if (ret == -EAGAIN || ret == -ETIMEDOUT) {
			if (++retries > THC_IPTS_REQ_TRIES ||
			    !wait_event_timeout(idev->wq,
						thc_ipts_idle(idev), timeout))
				break;
			continue;
		}
		if (ret)
			return ret;

		try++;
		wait_event_timeout(idev->wq, READ_ONCE(idev->have_desc),
				   timeout);
	}

	/* Pairs with the release in thc_ipts_descriptor(). */
	if (smp_load_acquire(&idev->have_desc) && idev->desc_len > 0) {
		/* The receive buffer holds more than a descriptor may be. */
		if (idev->desc_len > HID_MAX_DESCRIPTOR_SIZE) {
			dev_err(idev->dev,
				"report descriptor of %zu bytes is too large\n",
				idev->desc_len);
			return -EINVAL;
		}

		return hid_parse_report(hdev, idev->desc, idev->desc_len);
	}

	dev_err(idev->dev, "controller did not return a report descriptor\n");

	return -ETIMEDOUT;
}

/* A feature report comes back as a frame of its own: the request waits. */
static int thc_ipts_get_feature(struct thc_ipts *idev, unsigned char reportnum,
				u8 *buf, size_t len)
{
	unsigned long timeout = msecs_to_jiffies(THC_IPTS_REQ_TIMEOUT_MS);
	unsigned int seq;
	int ret;

	/*
	 * One request at a time, and the wait below happens without a lock: a
	 * request issued from a HID callback in the handler thread would be
	 * waiting for an answer only that very thread can deliver. The gate is
	 * released by whoever takes the answer, not by the frame that brings
	 * it, or a second request could be answered into this one's buffer.
	 */
	mutex_lock(&idev->feature_lock);

	/* The frame carries only the report ID, so a late answer cannot be
	 * told from the next one's: after a timeout the channel stays shut.
	 */
	if (idev->feature_off) {
		mutex_unlock(&idev->feature_lock);
		return -EIO;
	}
	if (idev->feature_pending) {
		mutex_unlock(&idev->feature_lock);
		return -EBUSY;
	}

	WRITE_ONCE(idev->feature_done, false);
	idev->feature_id = reportnum;
	idev->feature_pending = true;
	mutex_unlock(&idev->feature_lock);

	seq = READ_ONCE(idev->recover_seq);

	/* A refusal happens before anything is written, so the channel stays
	 * open for it; a timeout is a different matter, see below.
	 */
	ret = thc_ipts_send(idev, THC_IPTS_TX_GET_FEATURE, &reportnum, 1);
	if (ret) {
		mutex_lock(&idev->feature_lock);
		idev->feature_pending = false;

		/* The start bit was written before the wait, so the frame may
		 * well be on the wire: its answer cannot be told from the next
		 * request's one.
		 */
		if (ret == -ETIMEDOUT)
			WRITE_ONCE(idev->feature_off, true);
		mutex_unlock(&idev->feature_lock);

		if (ret == -ETIMEDOUT)
			dev_err(idev->dev, "feature report %u not acknowledged\n",
				reportnum);

		return ret;
	}

	wait_event_timeout(idev->wq, READ_ONCE(idev->feature_done) ||
			   READ_ONCE(idev->recover_pending) ||
			   READ_ONCE(idev->recovering) ||
			   READ_ONCE(idev->stopping), timeout);

	mutex_lock(&idev->feature_lock);
	/* An answer that landed while the wait was expiring counts too. */
	if (!idev->feature_done) {
		/* A reset discards what the engines had queued, so an answer
		 * that was on its way is gone and cannot be waited for.
		 */
		if (READ_ONCE(idev->recover_seq) != seq ||
		    READ_ONCE(idev->recover_pending) ||
		    READ_ONCE(idev->recovering) ||
		    READ_ONCE(idev->stopping))
			ret = -EIO;
		else
			ret = -ETIMEDOUT;

		idev->feature_pending = false;
		/* Only a timeout leaves a frame out there: after a reset the
		 * answer was dropped and the channel is still in step.
		 */
		if (ret == -ETIMEDOUT)
			WRITE_ONCE(idev->feature_off, true);
		mutex_unlock(&idev->feature_lock);

		if (ret == -ETIMEDOUT)
			dev_err(idev->dev, "no feature report %u\n", reportnum);

		return ret;
	}

	/* The payload is the report as user space writes it: ID first. */
	len = min(len, idev->feature_len);
	if (len)
		memcpy(buf, idev->feature, len);
	idev->feature_pending = false;
	mutex_unlock(&idev->feature_lock);

	return len ?: -EPROTO;
}

static int thc_ipts_hid_raw_request(struct hid_device *hdev,
				    unsigned char reportnum, __u8 *buf,
				    size_t len, unsigned char rtype,
				    int reqtype)
{
	struct thc_ipts *idev = hdev->driver_data;
	u32 code;
	int ret;

	if (!buf || !len)
		return -EINVAL;

	if (reqtype == HID_REQ_GET_REPORT && rtype == HID_FEATURE_REPORT)
		return thc_ipts_get_feature(idev, reportnum, buf, len);

	if (reqtype != HID_REQ_SET_REPORT ||
	    (rtype != HID_OUTPUT_REPORT && rtype != HID_FEATURE_REPORT))
		return -EOPNOTSUPP;

	code = rtype == HID_OUTPUT_REPORT ? THC_IPTS_TX_OUTPUT :
					    THC_IPTS_TX_SET_FEATURE;

	ret = thc_ipts_send(idev, code, buf, len);
	if (ret)
		return ret;

	/* raw_request returns the byte count, which hidraw forwards. */
	return (int)len;
}

/* The HID core calls these unconditionally; nothing to do in this mode. */
static int thc_ipts_hid_start(struct hid_device *hdev)
{
	return 0;
}

static void thc_ipts_hid_stop(struct hid_device *hdev)
{
}

static int thc_ipts_hid_open(struct hid_device *hdev)
{
	return 0;
}

static void thc_ipts_hid_close(struct hid_device *hdev)
{
}

static const struct hid_ll_driver thc_ipts_ll_driver = {
	.start		= thc_ipts_hid_start,
	.stop		= thc_ipts_hid_stop,
	.open		= thc_ipts_hid_open,
	.close		= thc_ipts_hid_close,
	.parse		= thc_ipts_hid_parse,
	.raw_request	= thc_ipts_hid_raw_request,
};

static void thc_ipts_hid_remove(struct thc_ipts *idev)
{
	if (!READ_ONCE(idev->hid))
		return;

	hid_destroy_device(idev->hid);
	WRITE_ONCE(idev->hid, NULL);
}

static int thc_ipts_hid_add(struct thc_ipts *idev)
{
	struct hid_device *hdev;
	int ret;

	hdev = hid_allocate_device();
	if (IS_ERR(hdev))
		return PTR_ERR(hdev);

	hdev->ll_driver = &thc_ipts_ll_driver;
	hdev->bus = BUS_PCI;
	hdev->vendor = idev->vendor;
	hdev->product = idev->product;
	hdev->version = le32_to_cpu(idev->cfg.revision) & 0xffff;
	hdev->dev.parent = idev->dev;
	hdev->driver_data = idev;
	snprintf(hdev->name, sizeof(hdev->name), "intel-ipts %04X:%04X",
		 idev->vendor, idev->product);
	strscpy(hdev->phys, dev_name(idev->dev), sizeof(hdev->phys));

	ret = hid_add_device(hdev);
	if (ret) {
		hid_destroy_device(hdev);
		return ret;
	}

	WRITE_ONCE(idev->hid, hdev);

	return 0;
}

/**
 * thc_ipts_port_start - bring one port up
 * @idev: port to start
 */
int thc_ipts_port_start(struct thc_ipts *idev)
{
	u32 ctrl, hz, mode;
	int ret;

	WRITE_ONCE(idev->stopping, false);
	WRITE_ONCE(idev->recover_pending, false);
	WRITE_ONCE(idev->recovering, false);
	WRITE_ONCE(idev->feature_off, false);

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

	/* Sets the type field to invalid; RXDMA2 start writes the real one. */
	thc_interrupt_config(idev->thc);

	ret = thc_ipts_dma_start(idev);
	if (ret)
		return ret;

	thc_interrupt_enable(idev->thc, true);

	return thc_ipts_hid_add(idev);
}

/**
 * thc_ipts_port_stop - tear one port down
 * @idev: port to stop
 */
void thc_ipts_port_stop(struct thc_ipts *idev)
{
	/* Refuse new requests first, then wait for one that is already inside
	 * thc_dma_write(): the completion it waits for comes through the
	 * handler this teardown is about to mask. The lock is not held across
	 * synchronize_irq(), because the threaded handler can issue a request
	 * of its own from a HID callback.
	 */
	WRITE_ONCE(idev->stopping, true);
	wake_up(&idev->wq);
	mutex_lock(&idev->tx_lock);
	mutex_unlock(&idev->tx_lock);

	thc_interrupt_enable(idev->thc, false);
	synchronize_irq(idev->irq);
	cancel_work_sync(&idev->recover_work);

	/* A handler that read ->stopping early may have unmasked. */
	thc_interrupt_enable(idev->thc, false);

	/* Stop the controller before its buffers go away. */
	if (idev->running)
		thc_ipts_touch_set(idev, false);

	idev->running = false;
	/* Forced: the bit may read as clear and still need the command. */
	regmap_write_bits(idev->thc->thc_regmap, THC_M_PRT_CONTROL_OFFSET,
			  THC_M_PRT_CONTROL_DEVRST, 0);

	/* The buffers are not devm-managed, so releasing them is the only way
	 * back: do it even when the engine did not acknowledge the stop, and
	 * let thc_dma_unconfigure() clear the start bits on its way out.
	 */
	if (thc_ipts_quiesce(idev, true))
		dev_warn(idev->dev, "engine did not stop, releasing anyway\n");
	thc_ipts_dma_stop(idev);

	/* ->dma_ready goes away with the buffers, and a handler woken before
	 * that still looks at the ring: wait for it once more, then mask what
	 * it may have left open.
	 */
	synchronize_irq(idev->irq);
	thc_interrupt_enable(idev->thc, false);

	thc_ipts_hid_remove(idev);
}
