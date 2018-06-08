// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2017-2018, The Linux foundation. All rights reserved.

#include <linux/clk.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/spi/spi.h>
#include <linux/spi/spi-mem.h>

#include <asm/unaligned.h>

#define AHB_MIN_HZ		9600000UL
#define QSPI_NUM_CS		2
#define QSPI_BYTES_PER_WORD	4
#define MSTR_CONFIG		0x0000
#define AHB_MASTER_CFG		0x0004
#define MSTR_INT_EN		0x000C
#define MSTR_INT_STATUS		0x0010
#define PIO_XFER_CTRL		0x0014
#define PIO_XFER_CFG		0x0018
#define PIO_XFER_STATUS		0x001c
#define PIO_DATAOUT_1B		0x0020
#define PIO_DATAOUT_4B		0x0024
#define RD_FIFO_CFG		0x0028
#define RD_FIFO_STATUS		0x002c
#define RD_FIFO_RESET		0x0030
#define CUR_MEM_ADDR		0x0048
#define HW_VERSION		0x004c
#define RD_FIFO			0x0050
#define SAMPLING_CLK_CFG	0x0090
#define SAMPLING_CLK_STATUS	0x0094

/* Macros to help set/get fields in MSTR_CONFIG register */
#define	FULL_CYCLE_MODE		BIT(3)
#define	FB_CLK_EN		BIT(4)
#define	PIN_HOLDN		BIT(6)
#define	PIN_WPN			BIT(7)
#define	DMA_ENABLE		BIT(8)
#define	BIG_ENDIAN_MODE		BIT(9)
#define	SPI_MODE_MSK		0xc00
#define	SPI_MODE_SHFT		10
#define	CHIP_SELECT_NUM		BIT(12)
#define	SBL_EN			BIT(13)
#define	LPA_BASE_MSK		0x3c000
#define	LPA_BASE_SHFT		14
#define	TX_DATA_DELAY_MSK	0xc0000
#define	TX_DATA_DELAY_SHFT	18
#define	TX_CLK_DELAY_MSK	0x300000
#define	TX_CLK_DELAY_SHFT	20
#define	TX_CS_N_DELAY_MSK	0xc00000
#define	TX_CS_N_DELAY_SHFT	22
#define	TX_DATA_OE_DELAY_MSK	0x3000000
#define	TX_DATA_OE_DELAY_SHFT	24

/* Macros to help set/get fields in AHB_MSTR_CFG register */
#define	HMEM_TYPE_START_MID_TRANS_MSK		0x7
#define	HMEM_TYPE_START_MID_TRANS_SHFT		0
#define	HMEM_TYPE_LAST_TRANS_MSK		0x38
#define	HMEM_TYPE_LAST_TRANS_SHFT		3
#define	USE_HMEMTYPE_LAST_ON_DESC_OR_CHAIN_MSK	0xc0
#define	USE_HMEMTYPE_LAST_ON_DESC_OR_CHAIN_SHFT	6
#define	HMEMTYPE_READ_TRANS_MSK			0x700
#define	HMEMTYPE_READ_TRANS_SHFT		8
#define	HSHARED					BIT(11)
#define	HINNERSHARED				BIT(12)

/* Macros to help set/get fields in MSTR_INT_EN/MSTR_INT_STATUS registers */
#define	RESP_FIFO_UNDERRUN	BIT(0)
#define	RESP_FIFO_NOT_EMPTY	BIT(1)
#define	RESP_FIFO_RDY		BIT(2)
#define	HRESP_FROM_NOC_ERR	BIT(3)
#define	WR_FIFO_EMPTY		BIT(9)
#define	WR_FIFO_FULL		BIT(10)
#define	WR_FIFO_OVERRUN		BIT(11)
#define	TRANSACTION_DONE	BIT(16)
#define QSPI_ERR_IRQS		(RESP_FIFO_UNDERRUN | HRESP_FROM_NOC_ERR | \
				 WR_FIFO_OVERRUN)
#define	QSPI_ALL_IRQS		(QSPI_ERR_IRQS | RESP_FIFO_RDY | \
				 WR_FIFO_EMPTY | WR_FIFO_FULL | \
				 TRANSACTION_DONE)

/* Macros to help set/get fields in RD_FIFO_CONFIG register */
#define	CONTINUOUS_MODE		BIT(0)

/* Macros to help set/get fields in RD_FIFO_RESET register */
#define	RESET_FIFO		BIT(0)

/* Macros to help set/get fields in PIO_TRANSFER_CONFIG register */
#define	TRANSFER_DIRECTION	BIT(0)
#define	MULTI_IO_MODE_MSK	0xe
#define	MULTI_IO_MODE_SHFT	1
#define	TRANSFER_FRAGMENT	BIT(8)

/* Macros to help set/get fields in PIO_TRANSFER_CONTROL register */
#define	REQUEST_COUNT_MSK	0xffff

/* Macros to help set/get fields in PIO_TRANSFER_STATUS register */
#define	WR_FIFO_BYTES_MSK	0xffff0000
#define	WR_FIFO_BYTES_SHFT	16

/* Macros to help set/get fields in RD_FIFO_STATUS register */
#define	FIFO_EMPTY	BIT(11)
#define	WR_CNTS_MSK	0x7f0
#define	WR_CNTS_SHFT	4
#define	RDY_64BYTE	BIT(3)
#define	RDY_32BYTE	BIT(2)
#define	RDY_16BYTE	BIT(1)
#define	FIFO_RDY	BIT(0)

/*
 * The Mode transfer macros, the values are programmed to the HW registers
 * when doing PIO mode of transfers.
 */
#define	SDR_1BIT	1
#define	SDR_2BIT	2
#define	SDR_4BIT	3
#define	DDR_1BIT	5
#define	DDR_2BIT	6
#define	DDR_4BIT	7

/* The Mode transfer macros when setting up DMA descriptors */
#define	DMA_DESC_SINGLE_SPI	1
#define	DMA_DESC_DUAL_SPI	2
#define	DMA_DESC_QUAD_SPI	3

enum qspi_dir {
	QSPI_READ,
	QSPI_WRITE,
};

struct qspi_xfer {
	union {
		const void *tx_buf;
		void *rx_buf;
	};
	unsigned int rem_bytes;
	int buswidth;
	enum qspi_dir dir;
	bool is_last;
	bool is_dummy;
};

enum qspi_clocks {
	QSPI_CLK_CORE,
	QSPI_CLK_IFACE,
	QSPI_NUM_CLKS
};

struct qcom_qspi {
	void __iomem *base;
	struct device *dev;
	struct clk_bulk_data clks[QSPI_NUM_CLKS];
	int irq;
	struct qspi_xfer xfer;
	struct completion transfer_complete;
};

static int qspi_buswidth_to_iomode(struct qcom_qspi *ctrl, int buswidth)
{
	switch (buswidth) {
	case 1:
		return SDR_1BIT;
	case 2:
		return SDR_2BIT;
	case 4:
		return SDR_4BIT;
	default:
		dev_warn_once(ctrl->dev,
			      "Unexpected bus width: %d\n", buswidth);
		return SDR_1BIT;
	}
}

static void qcom_qspi_pio_xfer_cfg(struct qcom_qspi *ctrl)
{
	u32 pio_xfer_cfg = 0;
	struct qspi_xfer *xfer;

	xfer = &ctrl->xfer;
	pio_xfer_cfg = readl(ctrl->base + PIO_XFER_CFG);
	pio_xfer_cfg &= ~TRANSFER_DIRECTION;
	pio_xfer_cfg |= xfer->dir;
	if (xfer->is_last)
		pio_xfer_cfg &= ~TRANSFER_FRAGMENT;
	else
		pio_xfer_cfg |= TRANSFER_FRAGMENT;
	pio_xfer_cfg &= ~MULTI_IO_MODE_MSK;
	pio_xfer_cfg |= qspi_buswidth_to_iomode(ctrl, xfer->buswidth) <<
			MULTI_IO_MODE_SHFT;

	writel(pio_xfer_cfg, ctrl->base + PIO_XFER_CFG);
}

static void qcom_qspi_pio_xfer_ctrl(struct qcom_qspi *ctrl)
{
	u32 pio_xfer_ctrl;

	pio_xfer_ctrl = readl(ctrl->base + PIO_XFER_CTRL);
	pio_xfer_ctrl &= ~REQUEST_COUNT_MSK;
	pio_xfer_ctrl |= ctrl->xfer.rem_bytes;
	writel(pio_xfer_ctrl, ctrl->base + PIO_XFER_CTRL);
}

static void qcom_qspi_pio_xfer(struct qcom_qspi *ctrl)
{
	u32 ints;

	qcom_qspi_pio_xfer_cfg(ctrl);

	/* Ack any previous interrupts that might be hanging around */
	writel(QSPI_ALL_IRQS, ctrl->base + MSTR_INT_STATUS);

	/* Setup new interrupts */
	if (ctrl->xfer.dir == QSPI_WRITE)
		ints = QSPI_ERR_IRQS | WR_FIFO_EMPTY;
	else
		ints = QSPI_ERR_IRQS | RESP_FIFO_RDY;
	writel(ints, ctrl->base + MSTR_INT_EN);

	/* Kick off the transfer */
	qcom_qspi_pio_xfer_ctrl(ctrl);
}

static int wait_for_xfer(struct qcom_qspi *ctrl)
{
	int ret = 0;

	if (!wait_for_completion_timeout(&ctrl->transfer_complete, HZ)) {
		ctrl->xfer.rem_bytes = 0;
		writel(0, ctrl->base + MSTR_INT_EN);
		ret = -ETIMEDOUT;
	}

	return ret;
}

static int process_opcode(const struct spi_mem_op *op, struct qcom_qspi *ctrl)
{
	ctrl->xfer.dir = QSPI_WRITE;
	ctrl->xfer.buswidth = op->cmd.buswidth;
	ctrl->xfer.is_last = !(op->addr.nbytes ||
			       op->dummy.nbytes || op->data.nbytes);
	ctrl->xfer.is_dummy = false;
	ctrl->xfer.rem_bytes = 1;
	ctrl->xfer.tx_buf = &op->cmd.opcode;
	qcom_qspi_pio_xfer(ctrl);

	return wait_for_xfer(ctrl);
}

static int process_addr(const struct spi_mem_op *op, struct qcom_qspi *ctrl)
{
	u8 buf[8];
	int bytes_left = op->addr.nbytes;
	u64 addr = op->addr.val;

	/* Convert so we send largest address byte first */
	if (bytes_left > ARRAY_SIZE(buf)) {
		dev_err(ctrl->dev, "Address too big: %d\n", bytes_left);
	}
	while (bytes_left) {
		bytes_left--;
		buf[bytes_left] = addr;
		addr >>= 8;
	}

	ctrl->xfer.dir = QSPI_WRITE;
	ctrl->xfer.buswidth = op->addr.buswidth;
	ctrl->xfer.is_last = !(op->dummy.nbytes || op->data.nbytes);
	ctrl->xfer.is_dummy = false;
	ctrl->xfer.rem_bytes = op->addr.nbytes;
	ctrl->xfer.tx_buf = buf;
	qcom_qspi_pio_xfer(ctrl);

	return wait_for_xfer(ctrl);
}

static int process_dummy(const struct spi_mem_op *op, struct qcom_qspi *ctrl)
{
	ctrl->xfer.dir = QSPI_WRITE;
	ctrl->xfer.buswidth = op->dummy.buswidth;
	ctrl->xfer.is_last = !op->data.nbytes;
	ctrl->xfer.is_dummy = true;
	ctrl->xfer.rem_bytes = op->dummy.nbytes;
	qcom_qspi_pio_xfer(ctrl);

	return wait_for_xfer(ctrl);
}

static int process_data(const struct spi_mem_op *op, struct qcom_qspi *ctrl)
{
	ctrl->xfer.dir = (op->data.dir == SPI_MEM_DATA_IN) ?
					QSPI_READ : QSPI_WRITE;
	ctrl->xfer.buswidth = op->data.buswidth;
	ctrl->xfer.is_last = true;
	ctrl->xfer.is_dummy = false;
	ctrl->xfer.rem_bytes = op->data.nbytes;

	if (ctrl->xfer.dir == QSPI_WRITE)
		ctrl->xfer.tx_buf = op->data.buf.out;
	else
		ctrl->xfer.rx_buf = op->data.buf.in;
	qcom_qspi_pio_xfer(ctrl);

	return wait_for_xfer(ctrl);
}

static int qcom_qspi_exec_mem_op(struct spi_mem *mem,
				const struct spi_mem_op *op)
{
	struct qcom_qspi *ctrl = spi_master_get_devdata(mem->spi->master);
	struct spi_device *dev = mem->spi;
	int ret;

	if (dev->max_speed_hz) {
		ret = clk_set_rate(ctrl->clks[QSPI_CLK_CORE].clk,
				   dev->max_speed_hz * 4);
		if (ret) {
			dev_err(ctrl->dev, "Failed to set core clk %d\n", ret);
			goto exit;
		}
	}

	ret = clk_bulk_prepare_enable(QSPI_NUM_CLKS, ctrl->clks);
	if (ret)
		return ret;

	trace_printk("DOUG: %s: addr (%#08x): %#06x dummy: %#06x data: %#06x\n",
		     __func__, op->addr.nbytes, (int) op->addr.val, op->dummy.nbytes, op->data.nbytes);

	ret = process_opcode(op, ctrl);
	if (ret)
		return ret;

	if (op->addr.nbytes) {
		ret = process_addr(op, ctrl);
		if (ret)
			goto exit;
	}

	if (op->dummy.nbytes) {
		ret = process_dummy(op, ctrl);
		if (ret)
			goto exit;
	}

	if (op->data.nbytes) {
		ret = process_data(op, ctrl);
		if (ret)
			goto exit;
	}

exit:
	clk_bulk_disable_unprepare(QSPI_NUM_CLKS, ctrl->clks);

	return ret;
}

static int qcom_qspi_setup(struct spi_device *spi)
{
	int ret;
	u32 mstr_cfg;
	struct qcom_qspi *ctrl;
	int tx_data_oe_delay = 1;	/* TODO: ??? */
	int tx_data_delay = 1;		/* TODO: ??? */

	ctrl = spi_master_get_devdata(spi->master);

	ret = clk_bulk_prepare_enable(QSPI_NUM_CLKS, ctrl->clks);
	if (ret)
		return ret;

	mstr_cfg = readl(ctrl->base + MSTR_CONFIG);
	mstr_cfg &= ~CHIP_SELECT_NUM;
	if (spi->chip_select)
		mstr_cfg |= CHIP_SELECT_NUM;

	/* TODO: Is Linux spi->mode the same format as our hardware expects? */
	mstr_cfg = (mstr_cfg & ~SPI_MODE_MSK) | (spi->mode << SPI_MODE_SHFT);
	mstr_cfg |= FB_CLK_EN | PIN_WPN | PIN_HOLDN | SBL_EN | FULL_CYCLE_MODE;
	mstr_cfg |= (mstr_cfg & ~TX_DATA_OE_DELAY_MSK) |
				(tx_data_oe_delay << TX_DATA_OE_DELAY_SHFT);
	mstr_cfg |= (mstr_cfg & ~TX_DATA_DELAY_MSK) |
				(tx_data_delay << TX_DATA_DELAY_SHFT);
	mstr_cfg &= ~DMA_ENABLE;

	writel(mstr_cfg, ctrl->base + MSTR_CONFIG);

	/*
	 * Ensure that the configuration goes through by reading back
	 * a register from the IO space.
	 */
	mstr_cfg = readl((ctrl->base + MSTR_CONFIG));

	clk_bulk_disable_unprepare(QSPI_NUM_CLKS, ctrl->clks);

	return 0;
}

static irqreturn_t pio_read(struct qcom_qspi *ctrl)
{
	u32 rd_fifo_status;
	u32 rd_fifo;
	unsigned int wr_cnts;
	unsigned int bytes_to_read;
	unsigned int words_to_read;
	u32 *word_buf;
	u8 *byte_buf;
	int i;

	rd_fifo_status = readl(ctrl->base + RD_FIFO_STATUS);

	if (!(rd_fifo_status & FIFO_RDY)) {
		dev_dbg(ctrl->dev, "Spurious IRQ %#x\n", rd_fifo_status);
		return IRQ_NONE;
	}

	wr_cnts = (rd_fifo_status & WR_CNTS_MSK) >> WR_CNTS_SHFT;

	if (wr_cnts > ctrl->xfer.rem_bytes)
		wr_cnts = ctrl->xfer.rem_bytes;

	words_to_read = wr_cnts / QSPI_BYTES_PER_WORD;
	bytes_to_read = wr_cnts % QSPI_BYTES_PER_WORD;

	if (words_to_read) {
		word_buf = ctrl->xfer.rx_buf;
		ctrl->xfer.rem_bytes -= words_to_read * QSPI_BYTES_PER_WORD;
		for (i = 0; i < words_to_read; i++) {
			rd_fifo = readl(ctrl->base + RD_FIFO);
			put_unaligned(rd_fifo, word_buf++);
		}
		ctrl->xfer.rx_buf = word_buf;
	}

	if (bytes_to_read) {
		byte_buf = ctrl->xfer.rx_buf;
		rd_fifo = readl(ctrl->base + RD_FIFO);
		ctrl->xfer.rem_bytes -= bytes_to_read;
		for (i = 0; i < bytes_to_read; i++)
			*byte_buf++ = rd_fifo >> (i * BITS_PER_BYTE);
		ctrl->xfer.rx_buf = byte_buf;
	}

	return IRQ_HANDLED;
}

static irqreturn_t pio_write(struct qcom_qspi *ctrl)
{
	const void *xfer_buf = ctrl->xfer.tx_buf;
	bool is_dummy = ctrl->xfer.is_dummy;
	const int *word_buf;
	const char *byte_buf;
	unsigned int wr_fifo_bytes;
	unsigned int wr_fifo_words;
	unsigned int wr_size;
	unsigned int rem_words;

	wr_fifo_bytes =
		readl(ctrl->base + PIO_XFER_STATUS) >> WR_FIFO_BYTES_SHFT;

	if (ctrl->xfer.rem_bytes < QSPI_BYTES_PER_WORD) {
		/* Process the last 1-3 bytes */
		wr_size = min(wr_fifo_bytes, ctrl->xfer.rem_bytes);
		ctrl->xfer.rem_bytes -= wr_size;

		if (is_dummy) {
			while (wr_size--)
				writel(0xff, ctrl->base + PIO_DATAOUT_1B);
		} else {
			byte_buf = xfer_buf;
			while (wr_size--)
				writel(*byte_buf++,
				       ctrl->base + PIO_DATAOUT_1B);
		}
		ctrl->xfer.tx_buf = byte_buf;
	} else {
		/*
		 * Process all the whole words; to keep things simple we'll
		 * just wait for the next interrupt to handle the last 1-3
		 * bytes if we don't have an even number of words.
		 */
		rem_words = ctrl->xfer.rem_bytes / QSPI_BYTES_PER_WORD;
		wr_fifo_words = wr_fifo_bytes / QSPI_BYTES_PER_WORD;

		wr_size = min(rem_words, wr_fifo_words);
		ctrl->xfer.rem_bytes -= wr_size * QSPI_BYTES_PER_WORD;

		if (is_dummy) {
			while (wr_size--)
				writel(0xffffffff, ctrl->base + PIO_DATAOUT_1B);
		} else {
			word_buf = xfer_buf;
			while (wr_size--)
				writel(get_unaligned(word_buf++),
				       ctrl->base + PIO_DATAOUT_4B);
		}
		ctrl->xfer.tx_buf = word_buf;

	}

	return IRQ_HANDLED;
}

static irqreturn_t qcom_qspi_irq(int irq, void *dev_id)
{
	u32 int_status;
	struct qcom_qspi *ctrl = dev_id;
	irqreturn_t ret;

	int_status = readl(ctrl->base + MSTR_INT_STATUS);
	writel(int_status, ctrl->base + MSTR_INT_STATUS);

	if ((int_status & WR_FIFO_EMPTY) && ctrl->xfer.dir == QSPI_WRITE)
		ret = pio_write(ctrl);

	if ((int_status & RESP_FIFO_RDY) && ctrl->xfer.dir == QSPI_READ)
		ret = pio_read(ctrl);

	if (!ctrl->xfer.rem_bytes) {
		writel(0, ctrl->base + MSTR_INT_EN);
		complete(&ctrl->transfer_complete);
	}

	return ret;
}

static int qcom_qspi_adjust_op_size(struct spi_mem *mem, struct spi_mem_op *op)
{
	if (op->data.nbytes > REQUEST_COUNT_MSK) {
		op->data.nbytes = REQUEST_COUNT_MSK;
	}

	return 0;
}

static const struct spi_controller_mem_ops qcom_qspi_mem_ops = {
	.exec_op = qcom_qspi_exec_mem_op,
	.adjust_op_size = qcom_qspi_adjust_op_size,
};

static int qcom_qspi_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct device *dev;
	struct resource *res;
	struct spi_master *master;
	struct qcom_qspi *ctrl;

	dev = &pdev->dev;

	master = spi_alloc_master(dev, sizeof(struct qcom_qspi));
	if (!master) {
		dev_err(dev, "Failed to alloc spi master\n");
		return -ENOMEM;
	}
	platform_set_drvdata(pdev, master);

	ctrl = spi_master_get_devdata(master);

	ctrl->dev = dev;
	init_completion(&ctrl->transfer_complete);
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	ctrl->base = devm_ioremap(dev, res->start, resource_size(res));
	if (IS_ERR(ctrl->base)) {
		ret = PTR_ERR(ctrl->base);
		dev_err(dev, "Failed to get base addr %d\n", ret);
		goto exit_probe_master_put;
	}

	ctrl->clks[QSPI_CLK_CORE].id = "core";
	ctrl->clks[QSPI_CLK_IFACE].id = "iface";
	ret = devm_clk_bulk_get(dev, QSPI_NUM_CLKS, ctrl->clks);
	if (ret)
		goto exit_probe_master_put;

	ctrl->irq = platform_get_irq(pdev, 0);
	if (ctrl->irq < 0) {
		ret = PTR_ERR(ctrl->base);
		dev_err(dev, "Failed to get irq %d\n", ret);
		goto exit_probe_master_put;
	}

	ret = devm_request_irq(dev, ctrl->irq, qcom_qspi_irq,
			IRQF_TRIGGER_HIGH, dev_name(dev), ctrl);
	if (ret) {
		dev_err(dev, "Failed to request irq %d\n", ret);
		goto exit_probe_master_put;
	}

	master->max_speed_hz = 300000000;
	master->num_chipselect = QSPI_NUM_CS;
	master->bus_num = pdev->id;
	master->dev.of_node = pdev->dev.of_node;
	master->mode_bits = SPI_MODE_0 | SPI_MODE_3 |
			    SPI_TX_DUAL | SPI_RX_DUAL |
			    SPI_TX_QUAD | SPI_RX_QUAD;
	master->setup = qcom_qspi_setup;
	master->mem_ops = &qcom_qspi_mem_ops;

	ret = devm_spi_register_master(dev, master);
	if (!ret)
		goto exit_probe_master_put;

	return 0;

exit_probe_master_put:
	spi_master_put(master);

	return ret;
}

static int qcom_qspi_resume(struct device *dev)
{
	return spi_master_resume(dev_get_drvdata(dev));
}

static int qcom_qspi_suspend(struct device *dev)
{
	return spi_master_suspend(dev_get_drvdata(dev));
}

static const struct dev_pm_ops qcom_qspi_dev_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(qcom_qspi_suspend, qcom_qspi_resume)
};

static const struct of_device_id qcom_qspi_dt_match[] = {
	{ .compatible = "qcom,qspi-v1", },
	{ }
};
MODULE_DEVICE_TABLE(of, qcom_qspi_dt_match);

static struct platform_driver qcom_qspi_driver = {
	.driver = {
		.name		= "qcom_qspi",
		.pm		= &qcom_qspi_dev_pm_ops,
		.of_match_table = qcom_qspi_dt_match,
	},
	.probe = qcom_qspi_probe,
};
module_platform_driver(qcom_qspi_driver);

MODULE_DESCRIPTION("SPI driver for QSPI cores");
MODULE_LICENSE("GPL v2");
