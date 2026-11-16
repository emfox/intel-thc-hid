/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _THC_IPTS_H_
#define _THC_IPTS_H_

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/build_bug.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/sched.h>
#include <linux/types.h>
#include <linux/wait.h>

#include "intel-thc-dev.h"

/* Configuration block the controller keeps at address zero. */
#define THC_IPTS_CFG_SIZE		64
#define THC_IPTS_CFG_TOUCH		0x0c
#define THC_IPTS_TIC_ID		0x43495424	/* "$TIC" */

/* Bulk data window. The DMA engines have to be pointed here, not at zero. */
#define THC_IPTS_BULK_ADDR		0x1000

#define PCI_DEVICE_ID_INTEL_THC_LKF_PORT1	0x98d0
#define PCI_DEVICE_ID_INTEL_THC_LKF_PORT2	0x98d1
#define PCI_DEVICE_ID_INTEL_THC_TGL_LP_PORT1	0xa0d0
#define PCI_DEVICE_ID_INTEL_THC_TGL_LP_PORT2	0xa0d1
#define PCI_DEVICE_ID_INTEL_THC_TGL_H_PORT1	0x43d0
#define PCI_DEVICE_ID_INTEL_THC_TGL_H_PORT2	0x43d1
#define PCI_DEVICE_ID_INTEL_THC_ADL_S_PORT1	0x7ad8
#define PCI_DEVICE_ID_INTEL_THC_ADL_S_PORT2	0x7ad9
#define PCI_DEVICE_ID_INTEL_THC_ADL_P_PORT1	0x51d0
#define PCI_DEVICE_ID_INTEL_THC_ADL_P_PORT2	0x51d1
#define PCI_DEVICE_ID_INTEL_THC_ADL_M_PORT1	0x54d0
#define PCI_DEVICE_ID_INTEL_THC_ADL_M_PORT2	0x54d1
#define PCI_DEVICE_ID_INTEL_THC_RPL_S_PORT1	0x7a58
#define PCI_DEVICE_ID_INTEL_THC_RPL_S_PORT2	0x7a59

/* The block fills in while the controller starts, so $TIC is retried. */
#define THC_IPTS_CFG_TRIES		10
#define THC_IPTS_CFG_RETRY_MS		100

#define THC_IPTS_CFG_KNOWN		40

/* The block as far as this driver reads it, padded to its full size. */
struct thc_ipts_cfg {
	__le32	irq_cause;
	__le32	error;		/* protocol error flags, not used */
	__le32	buf_sizes;	/* Rx/Tx sizes, see *_BUF_SIZE() */
	__le32	touch_cfg;	/* touch engine control, writable */
	__le32	touch_state;	/* touch engine state, not used */
	__le32	device_id;	/* THC_IPTS_TIC_ID after a good read */
	__le32	spi_cfg;
	__le16	vendor;
	__le16	product;
	__le32	revision;
	__le32	fw_version;	/* changes on every reset, not a version */
	u8	rest[THC_IPTS_CFG_SIZE - THC_IPTS_CFG_KNOWN];
};

static_assert(sizeof(struct thc_ipts_cfg) == THC_IPTS_CFG_SIZE);

/* The core is handed the struct itself, so the offsets are part of the deal. */
static_assert(offsetof(struct thc_ipts_cfg, buf_sizes) == 0x08);
static_assert(offsetof(struct thc_ipts_cfg, touch_cfg) == 0x0c);
static_assert(offsetof(struct thc_ipts_cfg, spi_cfg) == 0x18);
static_assert(offsetof(struct thc_ipts_cfg, vendor) == 0x1c);

/* Buffer sizes are stored as (size / 64) - 1, Rx in bits 13:0, Tx in 23:14. */
#define THC_IPTS_DMA_UNIT		64
#define THC_IPTS_RX_BUF_SIZE(val)	\
	((size_t)(FIELD_GET(GENMASK(13, 0), (val)) + 1) * THC_IPTS_DMA_UNIT)
#define THC_IPTS_TX_BUF_SIZE(val)	\
	((size_t)(FIELD_GET(GENMASK(23, 14), (val)) + 1) * THC_IPTS_DMA_UNIT)

#define THC_IPTS_TOUCH_ENABLE		BIT(0)
#define THC_IPTS_TOUCH_HID_REPORT	BIT(2)
#define THC_IPTS_TOUCH_POWER_ON	3
#define THC_IPTS_TOUCH_POWER_MASK	GENMASK(5, 3)
#define THC_IPTS_TOUCH_POWER(val)	\
	FIELD_PREP(THC_IPTS_TOUCH_POWER_MASK, (val))

#define THC_IPTS_SPI_CLKDIV(val)	FIELD_GET(GENMASK(3, 1), (val))
#define THC_IPTS_SPI_CLKDIV8		BIT(4)
#define THC_IPTS_SPI_DUAL		BIT(6)
#define THC_IPTS_SPI_QUAD		BIT(7)
/* Slowest gear in the core's SPI table; anything under it is refused. */
#define THC_IPTS_SPI_MIN_HZ		7812500

/* Code and length in front of the payload; the long one is inbound. */
#define THC_IPTS_RX_HDR_SIZE		64
#define THC_IPTS_TX_HDR_SIZE		8

/**
 * enum thc_ipts_state - how far the port has been brought up
 * @THC_IPTS_DOWN: no controller, no buffers, no HID device
 * @THC_IPTS_RESET: out of reset, configuration not read yet
 * @THC_IPTS_CONFIG: configuration read, engines not configured
 * @THC_IPTS_DMA: DMA engines configured and may be written to
 * @THC_IPTS_UP: HID device registered
 *
 * The steps only move forward while the port starts and only backward while
 * it stops. A failure therefore leaves the port at the last step that
 * completed, and the teardown knows how far to unwind.
 */
enum thc_ipts_state {
	THC_IPTS_DOWN,
	THC_IPTS_RESET,
	THC_IPTS_CONFIG,
	THC_IPTS_DMA,
	THC_IPTS_UP,
};

/**
 * struct thc_ipts - state of one THC port in IPTS mode
 * @dev: device used for resources and logging
 * @thc: shared THC core context
 * @irq: interrupt line of the port
 * @cfg: configuration block last read from the controller
 * @vendor: vendor ID the controller reports for its HID device
 * @product: product ID the controller reports for its HID device
 * @max_rx: largest frame the controller may send, in bytes
 * @max_tx: largest frame the driver may send, in bytes
 * @buf_rx: allocated size of @rx_frame
 * @rx_frame: buffer for one received frame, including its header
 * @recover_work: resets the receive engine after a transfer error
 * @state: how far the port has been brought up, see enum thc_ipts_state
 * @recover_pending: a reset is wanted, refuse new requests
 * @recovering: a reset is queued or running, ignore the interrupt
 * @recover_seq: number of resets that ran, to tell a dropped answer apart
 * @stopping: the port is being torn down, ignore the interrupt
 * @irq_thread: thread the port interrupt is handled in, once it has run
 */
struct thc_ipts {
	struct device		*dev;
	struct thc_device	*thc;
	int			irq;
	struct thc_ipts_cfg	cfg;
	u16			vendor;
	u16			product;
	size_t			max_rx;
	size_t			max_tx;
	size_t			buf_rx;
	u8			*rx_frame;
	struct work_struct	recover_work;
	enum thc_ipts_state	state;
	bool			recover_pending;
	bool			recovering;
	unsigned int		recover_seq;
	bool			stopping;
	struct task_struct	*irq_thread;
};

irqreturn_t thc_ipts_hardirq(int irq, void *data);
irqreturn_t thc_ipts_thread(int irq, void *data);
void thc_ipts_recover(struct work_struct *work);

int thc_ipts_port_start(struct thc_ipts *idev);
void thc_ipts_port_stop(struct thc_ipts *idev);

#endif /* _THC_IPTS_H_ */
