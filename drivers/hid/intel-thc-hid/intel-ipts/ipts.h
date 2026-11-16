/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _THC_IPTS_H_
#define _THC_IPTS_H_

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/build_bug.h>
#include <linux/device.h>
#include <linux/hid.h>
#include <linux/interrupt.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
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

/* A report descriptor arrives with eight bytes of preamble in front of it. */
#define THC_IPTS_DESC_PREAMBLE	8

/* Retry budget for the report descriptor handshake. */
#define THC_IPTS_REQ_TRIES		6
#define THC_IPTS_REQ_TIMEOUT_MS	300

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
	__le32	fw_version;
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
#define THC_IPTS_SPI_CLKDIV8_DIV	8
#define THC_IPTS_SPI_DUAL		BIT(6)
#define THC_IPTS_SPI_QUAD		BIT(7)
#define THC_IPTS_SPI_BASE_HZ		125000000
/* Slowest gear in the core's SPI table; anything under it is refused. */
#define THC_IPTS_SPI_MIN_HZ		7812500

/* Code and length in front of the payload; the long one is inbound. */
#define THC_IPTS_RX_HDR_SIZE		64
#define THC_IPTS_TX_HDR_SIZE		8

/* Frame codes. The same numbers mean different things per direction. */
#define THC_IPTS_RX_INPUT		3
#define THC_IPTS_RX_FEATURE		4
#define THC_IPTS_RX_DESC		5
#define THC_IPTS_RX_RESET		7

#define THC_IPTS_TX_SET_FEATURE	3
#define THC_IPTS_TX_GET_FEATURE	4
#define THC_IPTS_TX_OUTPUT		5
#define THC_IPTS_TX_GET_DESC		7

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
 * @buf_tx: allocated size of @tx_frame
 * @buf_desc: allocated size of @desc
 * @buf_feat: allocated size of @feature
 * @rx_frame: buffer for one received frame, including its header
 * @tx_frame: buffer for one frame to send, including its header
 * @desc: copy of the report descriptor, past its preamble
 * @desc_len: length of @desc
 * @have_desc: a descriptor arrived and has not been consumed yet
 * @feature: buffer a feature report is collected in
 * @feature_len: length of the report in @feature
 * @feature_id: report ID that was asked for, checked against the answer
 * @feature_pending: a feature report was asked for and has not arrived yet
 * @feature_done: a feature report arrived and has not been consumed yet
 * @feature_off: a request timed out and no more are taken until the port
 *	restarts
 * @feature_lock: protects the feature report buffer and its flags
 * @wq: waited on for a report descriptor or a feature report
 * @tx_lock: serialises writes into @tx_frame
 * @hid: HID device the reports are handed to, once it has been registered
 * @recover_work: resets the receive engine after a transfer error
 * @running: the controller answered and is out of reset
 * @dma_ready: DMA engines are configured and may be used
 * @recover_pending: a reset is wanted, refuse new requests
 * @recovering: a reset is queued or running, ignore the interrupt
 * @recover_seq: number of resets that ran, to tell a dropped answer apart
 * @stopping: the port is being torn down, ignore the interrupt
 * @in_thread: the threaded handler is running, so no write can complete
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
	size_t			buf_tx;
	size_t			buf_desc;
	size_t			buf_feat;
	u8			*rx_frame;
	u8			*tx_frame;
	u8			*desc;
	size_t			desc_len;
	bool			have_desc;
	u8			*feature;
	size_t			feature_len;
	u8			feature_id;
	bool			feature_pending;
	bool			feature_done;
	bool			feature_off;
	struct mutex		feature_lock;	/* feature report handshake */
	wait_queue_head_t	wq;
	struct mutex		tx_lock;	/* transmit frame */
	struct hid_device	*hid;
	struct work_struct	recover_work;
	bool			running;
	bool			dma_ready;
	bool			recover_pending;
	bool			recovering;
	unsigned int		recover_seq;
	bool			stopping;
	bool			in_thread;
};

irqreturn_t thc_ipts_hardirq(int irq, void *data);
irqreturn_t thc_ipts_thread(int irq, void *data);
void thc_ipts_recover(struct work_struct *work);

int thc_ipts_port_start(struct thc_ipts *idev);
void thc_ipts_port_stop(struct thc_ipts *idev);

#endif /* _THC_IPTS_H_ */
