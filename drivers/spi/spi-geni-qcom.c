// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2017-2018, The Linux foundation. All rights reserved.

#include <linux/clk.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/pm_runtime.h>
#include <linux/spinlock.h>
#include <linux/qcom-geni-se.h>
#include <linux/spi/spi.h>
#include <linux/spi/spi-geni-qcom.h>

#define SPI_NUM_CHIPSELECT	4
#define SPI_XFER_TIMEOUT_MS	250
/* SPI SE specific registers */
#define SE_SPI_CPHA		0x224
#define SE_SPI_LOOPBACK		0x22c
#define SE_SPI_CPOL		0x230
#define SE_SPI_DEMUX_OUTPUT_INV	0x24c
#define SE_SPI_DEMUX_SEL	0x250
#define SE_SPI_TRANS_CFG	0x25c
#define SE_SPI_WORD_LEN		0x268
#define SE_SPI_TX_TRANS_LEN	0x26c
#define SE_SPI_RX_TRANS_LEN	0x270
#define SE_SPI_PRE_POST_CMD_DLY	0x274
#define SE_SPI_DELAY_COUNTERS	0x278

/* SE_SPI_CPHA register fields */
#define CPHA			BIT(0)

/* SE_SPI_LOOPBACK register fields */
#define LOOPBACK_ENABLE		0x1
#define NORMAL_MODE		0x0
#define LOOPBACK_MSK		GENMASK(1, 0)

/* SE_SPI_CPOL register fields */
#define CPOL			BIT(2)

/* SE_SPI_DEMUX_OUTPUT_INV register fields */
#define CS_DEMUX_OUTPUT_INV_MSK	GENMASK(3, 0)

/* SE_SPI_DEMUX_SEL register fields */
#define CS_DEMUX_OUTPUT_SEL	GENMASK(3, 0)

/* SE_SPI_TX_TRANS_CFG register fields */
#define CS_TOGGLE		BIT(0)

/* SE_SPI_WORD_LEN register fields */
#define WORD_LEN_MSK		GENMASK(9, 0)
#define MIN_WORD_LEN		4

/* SPI_TX/SPI_RX_TRANS_LEN fields */
#define TRANS_LEN_MSK		GENMASK(23, 0)

/* SE_SPI_DELAY_COUNTERS */
#define SPI_INTER_WORDS_DELAY_MSK	GENMASK(9, 0)
#define SPI_CS_CLK_DELAY_MSK		GENMASK(19, 10)
#define SPI_CS_CLK_DELAY_SHFT		10

/* M_CMD OP codes for SPI */
#define SPI_TX_ONLY		1
#define SPI_RX_ONLY		2
#define SPI_FULL_DUPLEX		3
#define SPI_TX_RX		7
#define SPI_CS_ASSERT		8
#define SPI_CS_DEASSERT		9
#define SPI_SCK_ONLY		10
/* M_CMD params for SPI */
#define SPI_PRE_CMD_DELAY	BIT(0)
#define TIMESTAMP_BEFORE	BIT(1)
#define FRAGMENTATION		BIT(2)
#define TIMESTAMP_AFTER		BIT(3)
#define POST_CMD_DELAY		BIT(4)

static irqreturn_t geni_spi_isr(int irq, void *dev);

struct spi_geni_master {
	struct geni_se se;
	int irq;
	struct device *dev;
	int rx_fifo_depth;
	int tx_fifo_depth;
	int tx_fifo_width;
	int tx_wm;
	bool setup;
	u32 cur_speed_hz;
	int cur_word_len;
	spinlock_t lock;
	unsigned int tx_rem_bytes;
	unsigned int rx_rem_bytes;
	struct spi_transfer *cur_xfer;
	struct completion xfer_done;
	int oversampling;
	int suspended;
};

static struct spi_master *get_spi_master(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct spi_master *spi = platform_get_drvdata(pdev);

	return spi;
}

static int get_spi_clk_cfg(u32 speed_hz, struct spi_geni_master *mas,
			int *clk_idx, int *clk_div)
{
	unsigned long sclk_freq;
	struct geni_se *se = &mas->se;
	int ret;

	ret = geni_se_clk_freq_match(se,
				(speed_hz * mas->oversampling), clk_idx,
				&sclk_freq, true);
	if (ret) {
		dev_err(mas->dev, "%s: Failed(%d) to find src clk for 0x%x\n",
						__func__, ret, speed_hz);
		return ret;
	}

	sclk_freq = 19200000;
	*clk_idx = 0;
	*clk_div = ((sclk_freq / mas->oversampling) / speed_hz);
	if (!(*clk_div)) {
		dev_err(mas->dev, "%s:Err:sclk:%lu oversampling:%d speed:%u\n",
			__func__, sclk_freq, mas->oversampling, speed_hz);
		return -EINVAL;
	}

	dev_dbg(mas->dev, "%s: req %u sclk %lu, idx %d, div %d\n", __func__,
				speed_hz, sclk_freq, *clk_idx, *clk_div);
	//ret = clk_set_rate(se->clk, sclk_freq);
	if (ret)
		dev_err(mas->dev, "%s: clk_set_rate failed %d\n",
							__func__, ret);
	return ret;
}

static void spi_setup_word_len(struct spi_geni_master *mas, u32 mode,
						int bits_per_word)
{
	int pack_words = 1;
	bool msb_first = (mode & SPI_LSB_FIRST) ? false : true;
	struct geni_se *se = &mas->se;
	u32 word_len;

	word_len = readl_relaxed(se->base + SE_SPI_WORD_LEN);

	/*
	 * If bits_per_word isn't a byte aligned value, set the packing to be
	 * 1 SPI word per FIFO word.
	 */
	if (!(mas->tx_fifo_width % bits_per_word))
		pack_words = mas->tx_fifo_width / bits_per_word;
	word_len &= ~WORD_LEN_MSK;
	word_len |= ((bits_per_word - MIN_WORD_LEN) & WORD_LEN_MSK);
	geni_se_config_packing(&mas->se, bits_per_word, pack_words, msb_first,
								true, true);
	writel_relaxed(word_len, se->base + SE_SPI_WORD_LEN);
}

static int setup_fifo_params(struct spi_device *spi_slv,
					struct spi_master *spi)
{
	struct spi_geni_master *mas = spi_master_get_devdata(spi);
	struct geni_se *se = &mas->se;
	u16 mode = spi_slv->mode;
	u32 loopback_cfg = readl_relaxed(se->base + SE_SPI_LOOPBACK);
	u32 cpol = readl_relaxed(se->base + SE_SPI_CPOL);
	u32 cpha = readl_relaxed(se->base + SE_SPI_CPHA);
	u32 demux_sel = 0;
	u32 demux_output_inv = 0;
	u32 clk_sel = 0;
	u32 m_clk_cfg = 0;
	int ret = 0;
	int idx;
	int div;
	struct spi_geni_qcom_ctrl_data *delay_params = NULL;
	u32 spi_delay_params = 0;

	loopback_cfg &= ~LOOPBACK_MSK;
	cpol &= ~CPOL;
	cpha &= ~CPHA;

	if (mode & SPI_LOOP)
		loopback_cfg |= LOOPBACK_ENABLE;

	if (mode & SPI_CPOL)
		cpol |= CPOL;

	if (mode & SPI_CPHA)
		cpha |= CPHA;

	if (spi_slv->mode & SPI_CS_HIGH)
		demux_output_inv |= BIT(spi_slv->chip_select);

	if (spi_slv->controller_data) {
		u32 cs_clk_delay = 0;
		u32 inter_words_delay = 0;

		delay_params =
		(struct spi_geni_qcom_ctrl_data *) spi_slv->controller_data;
		cs_clk_delay =
		(delay_params->spi_cs_clk_delay << SPI_CS_CLK_DELAY_SHFT)
							& SPI_CS_CLK_DELAY_MSK;
		inter_words_delay =
			delay_params->spi_inter_words_delay &
						SPI_INTER_WORDS_DELAY_MSK;
		spi_delay_params =
		(inter_words_delay | cs_clk_delay);
	}

	demux_sel = spi_slv->chip_select;
	mas->cur_speed_hz = spi_slv->max_speed_hz;
	mas->cur_word_len = spi_slv->bits_per_word;

	ret = get_spi_clk_cfg(mas->cur_speed_hz, mas, &idx, &div);
	if (ret) {
		dev_err(mas->dev, "Err setting clks ret(%d) for %d\n",
							ret, mas->cur_speed_hz);
		goto setup_fifo_params_exit;
	}

	clk_sel |= (idx & CLK_SEL_MSK);
	m_clk_cfg |= ((div << CLK_DIV_SHFT) | SER_CLK_EN);
	spi_setup_word_len(mas, spi_slv->mode, spi_slv->bits_per_word);
	writel_relaxed(loopback_cfg, se->base + SE_SPI_LOOPBACK);
	writel_relaxed(demux_sel, se->base + SE_SPI_DEMUX_SEL);
	writel_relaxed(cpha, se->base + SE_SPI_CPHA);
	writel_relaxed(cpol, se->base + SE_SPI_CPOL);
	writel_relaxed(demux_output_inv, se->base + SE_SPI_DEMUX_OUTPUT_INV);
	writel_relaxed(clk_sel, se->base + SE_GENI_CLK_SEL);
	writel_relaxed(m_clk_cfg, se->base + GENI_SER_M_CLK_CFG);
	writel_relaxed(spi_delay_params, se->base + SE_SPI_DELAY_COUNTERS);
setup_fifo_params_exit:
	return ret;
}

static int spi_geni_prepare_message(struct spi_master *spi,
					struct spi_message *spi_msg)
{
	int ret = 0;
	struct spi_geni_master *mas = spi_master_get_devdata(spi);
	struct geni_se *se = &mas->se;

	geni_se_select_mode(se, GENI_SE_FIFO);
	reinit_completion(&mas->xfer_done);
	ret = setup_fifo_params(spi_msg->spi, spi);
	if (ret) {
		dev_err(mas->dev, "%s: Couldn't select mode %d", __func__, ret);
		ret = -EINVAL;
	}
	return ret;
}

static int spi_geni_unprepare_message(struct spi_master *spi_mas,
					struct spi_message *spi_msg)
{
	struct spi_geni_master *mas = spi_master_get_devdata(spi_mas);

	mas->cur_speed_hz = 0;
	mas->cur_word_len = 0;
	return 0;
}

static int spi_geni_prepare_transfer_hardware(struct spi_master *spi)
{
	struct spi_geni_master *mas = spi_master_get_devdata(spi);
	int ret = 0;
	struct geni_se *se = &mas->se;

	ret = pm_runtime_get_sync(mas->dev);
	if (ret < 0) {
		dev_err(mas->dev, "Error enabling SE resources\n");
		pm_runtime_put_noidle(mas->dev);
		goto exit_prepare_transfer_hardware;
	} else {
		ret = 0;
	}

	if (unlikely(!mas->setup)) {
		int proto = geni_se_read_proto(se);
		u32 ver;
		unsigned int major;
		unsigned int minor;

		if (unlikely(proto != GENI_SE_SPI)) {
			dev_err(mas->dev, "Invalid proto %d\n", proto);
			return -ENXIO;
		}
		mas->tx_fifo_depth = geni_se_get_tx_fifo_depth(se);
		mas->rx_fifo_depth = geni_se_get_rx_fifo_depth(se);
		mas->tx_fifo_width = geni_se_get_tx_fifo_width(se);
		geni_se_init(se, 0x0, (mas->tx_fifo_depth - 2));
		mas->oversampling = 1;
		/* Transmit an entire FIFO worth of data per IRQ */
		mas->tx_wm = 1;
		ver = geni_se_get_qup_hw_version(se);
		major = GENI_SE_VERSION_MAJOR(ver);
		minor = GENI_SE_VERSION_MINOR(ver);
		if ((major == 1) && (minor == 0))
			mas->oversampling = 2;
		mas->setup = 1;
		ret = devm_request_irq(mas->dev, mas->irq, geni_spi_isr,
			       IRQF_TRIGGER_HIGH, "spi_geni", mas);
		if (ret) {
			dev_err(mas->dev, "Request_irq failed:%d: err:%d\n",
				   mas->irq, ret);
			goto exit_prepare_transfer_hardware;
		}
	}
exit_prepare_transfer_hardware:
	return ret;
}

static int spi_geni_unprepare_transfer_hardware(struct spi_master *spi)
{
	struct spi_geni_master *mas = spi_master_get_devdata(spi);

	pm_runtime_put_sync(mas->dev);
	return 0;
}

static void setup_fifo_xfer(struct spi_transfer *xfer,
				struct spi_geni_master *mas, u16 mode,
				struct spi_master *spi)
{
	u32 m_cmd = 0;
	u32 m_param = 0;
	struct geni_se *se = &mas->se;
	u32 spi_tx_cfg = readl_relaxed(se->base + SE_SPI_TRANS_CFG);
	u32 trans_len = 0;

	if (xfer->bits_per_word != mas->cur_word_len) {
		spi_setup_word_len(mas, mode, xfer->bits_per_word);
		mas->cur_word_len = xfer->bits_per_word;
	}

	/* Speed and bits per word can be overridden per transfer */
	if (xfer->speed_hz != mas->cur_speed_hz) {
		int ret = 0;
		u32 clk_sel = 0;
		u32 m_clk_cfg = 0;
		int idx = 0;
		int div = 0;

		ret = get_spi_clk_cfg(xfer->speed_hz, mas, &idx, &div);
		if (ret) {
			dev_err(mas->dev, "%s:Err setting clks:%d\n",
								__func__, ret);
			return;
		}
		mas->cur_speed_hz = xfer->speed_hz;
		clk_sel |= (idx & CLK_SEL_MSK);
		m_clk_cfg |= ((div << CLK_DIV_SHFT) | SER_CLK_EN);
		writel_relaxed(clk_sel, se->base + SE_GENI_CLK_SEL);
		writel_relaxed(m_clk_cfg, se->base + GENI_SER_M_CLK_CFG);
	}

	mas->tx_rem_bytes = 0;
	mas->rx_rem_bytes = 0;
	if (xfer->tx_buf && xfer->rx_buf)
		m_cmd = SPI_FULL_DUPLEX;
	else if (xfer->tx_buf)
		m_cmd = SPI_TX_ONLY;
	else if (xfer->rx_buf)
		m_cmd = SPI_RX_ONLY;

	spi_tx_cfg &= ~CS_TOGGLE;
	if (!(mas->cur_word_len % MIN_WORD_LEN)) {
		trans_len =
			((xfer->len * BITS_PER_BYTE) /
					mas->cur_word_len) & TRANS_LEN_MSK;
	} else {
		int bytes_per_word = (mas->cur_word_len / BITS_PER_BYTE) + 1;

		trans_len = (xfer->len / bytes_per_word) & TRANS_LEN_MSK;
	}

	/*
	 * If CS change flag is set, then toggle the CS line in between
	 * transfers and keep CS asserted after the last transfer.
	 * Else if keep CS flag asserted in between transfers and de-assert
	 * CS after the last message.
	 */
	if (xfer->cs_change) {
		if (list_is_last(&xfer->transfer_list,
				&spi->cur_msg->transfers))
			m_param |= FRAGMENTATION;
	} else {
		if (!list_is_last(&xfer->transfer_list,
				&spi->cur_msg->transfers))
			m_param |= FRAGMENTATION;
	}

	mas->cur_xfer = xfer;
	if (m_cmd & SPI_TX_ONLY) {
		mas->tx_rem_bytes = xfer->len;
		writel_relaxed(trans_len, se->base + SE_SPI_TX_TRANS_LEN);
	}

	if (m_cmd & SPI_RX_ONLY) {
		writel_relaxed(trans_len, se->base + SE_SPI_RX_TRANS_LEN);
		mas->rx_rem_bytes = xfer->len;
	}
	writel_relaxed(spi_tx_cfg, se->base + SE_SPI_TRANS_CFG);
	geni_se_setup_m_cmd(se, m_cmd, m_param);
	if (m_cmd & SPI_TX_ONLY)
		writel_relaxed(mas->tx_wm, se->base + SE_GENI_TX_WATERMARK_REG);
}

static void handle_fifo_timeout(struct spi_geni_master *mas)
{
	unsigned long timeout;
	struct geni_se *se = &mas->se;
	unsigned long flags;

	reinit_completion(&mas->xfer_done);
	spin_lock_irqsave(&mas->lock, flags);
	geni_se_cancel_m_cmd(se);
	writel_relaxed(0, se->base + SE_GENI_TX_WATERMARK_REG);
	timeout = wait_for_completion_timeout(&mas->xfer_done, HZ);
	if (!timeout) {
		reinit_completion(&mas->xfer_done);
		geni_se_abort_m_cmd(se);
		timeout = wait_for_completion_timeout(&mas->xfer_done,
								HZ);
		if (!timeout)
			dev_err(mas->dev,
				"Failed to cancel/abort m_cmd\n");
	}
	spin_unlock_irqrestore(&mas->lock, flags);
}

static int spi_geni_transfer_one(struct spi_master *spi,
				struct spi_device *slv,
				struct spi_transfer *xfer)
{
	int ret = 0;
	struct spi_geni_master *mas = spi_master_get_devdata(spi);
	unsigned long timeout;

	WARN_ON(mas->suspended);

	if ((xfer->tx_buf == NULL) && (xfer->rx_buf == NULL)) {
		dev_err(mas->dev, "Invalid xfer both tx rx are NULL\n");
		return -EINVAL;
	}

	setup_fifo_xfer(xfer, mas, slv->mode, spi);
	timeout = wait_for_completion_timeout(&mas->xfer_done,
				msecs_to_jiffies(SPI_XFER_TIMEOUT_MS));
	if (!timeout) {
		dev_err(mas->dev,
			"Xfer[len %d tx %pK rx %pK n %d] timed out.\n",
					xfer->len, xfer->tx_buf,
					xfer->rx_buf,
					xfer->bits_per_word);
		mas->cur_xfer = NULL;
		ret = -ETIMEDOUT;
		goto err_fifo_geni_transfer_one;
	}
	return ret;
err_fifo_geni_transfer_one:
	handle_fifo_timeout(mas);
	return ret;
}

static void geni_spi_handle_tx(struct spi_geni_master *mas)
{
	int i = 0;
	int tx_fifo_width = mas->tx_fifo_width / BITS_PER_BYTE;
	int max_bytes = 0;
	const u8 *tx_buf;
	struct geni_se *se = &mas->se;

	if (!mas->cur_xfer)
		return;

	/*
	 * For non-byte aligned bits-per-word values. (e.g 9)
	 * The FIFO packing is set to 1 SPI word per FIFO word.
	 * Assumption is that each SPI word will be accomodated in
	 * ceil (bits_per_word / bits_per_byte)
	 * and the next SPI word starts at the next byte.
	 * In such cases, we can fit 1 SPI word per FIFO word so adjust the
	 * max byte that can be sent per IRQ accordingly.
	 */
	if ((mas->tx_fifo_width % mas->cur_word_len))
		max_bytes = (mas->tx_fifo_depth - mas->tx_wm) *
				((mas->cur_word_len / BITS_PER_BYTE) + 1);
	else
		max_bytes = (mas->tx_fifo_depth - mas->tx_wm) * tx_fifo_width;
	tx_buf = mas->cur_xfer->tx_buf;
	tx_buf += (mas->cur_xfer->len - mas->tx_rem_bytes);
	max_bytes = min_t(int, mas->tx_rem_bytes, max_bytes);
	while (i < max_bytes) {
		int j;
		u32 fifo_word = 0;
		u8 *fifo_byte;
		int bytes_per_fifo = tx_fifo_width;
		int bytes_to_write = 0;

		if ((mas->tx_fifo_width % mas->cur_word_len))
			bytes_per_fifo =
				(mas->cur_word_len / BITS_PER_BYTE) + 1;
		bytes_to_write = min_t(int, max_bytes - i, bytes_per_fifo);
		fifo_byte = (u8 *)&fifo_word;
		for (j = 0; j < bytes_to_write; j++)
			fifo_byte[j] = tx_buf[i++];
		iowrite32_rep(se->base + SE_GENI_TX_FIFOn, &fifo_word, 1);
	}
	mas->tx_rem_bytes -= max_bytes;
	if (!mas->tx_rem_bytes)
		writel_relaxed(0, se->base + SE_GENI_TX_WATERMARK_REG);
}

static void geni_spi_handle_rx(struct spi_geni_master *mas)
{
	int i = 0;
	struct geni_se *se = &mas->se;
	int fifo_width = mas->tx_fifo_width / BITS_PER_BYTE;
	u32 rx_fifo_status = readl_relaxed(se->base + SE_GENI_RX_FIFO_STATUS);
	int rx_bytes = 0;
	int rx_wc = 0;
	u8 *rx_buf;

	if (!mas->cur_xfer)
		return;

	rx_buf = mas->cur_xfer->rx_buf;
	rx_wc = rx_fifo_status & RX_FIFO_WC_MSK;
	if (rx_fifo_status & RX_LAST) {
		int rx_last_byte_valid =
			(rx_fifo_status & RX_LAST_BYTE_VALID_MSK)
					>> RX_LAST_BYTE_VALID_SHFT;
		if (rx_last_byte_valid && (rx_last_byte_valid < 4)) {
			rx_wc -= 1;
			rx_bytes += rx_last_byte_valid;
		}
	}

	/*
	 * For non-byte aligned bits-per-word values. (e.g 9)
	 * The FIFO packing is set to 1 SPI word per FIFO word.
	 * Assumption is that each SPI word will be accomodated in
	 * ceil (bits_per_word / bits_per_byte)
	 * and the next SPI word starts at the next byte.
	 */
	if (!(mas->tx_fifo_width % mas->cur_word_len))
		rx_bytes += rx_wc * fifo_width;
	else
		rx_bytes += rx_wc *
			((mas->cur_word_len / BITS_PER_BYTE) + 1);
	rx_bytes = min_t(int, mas->rx_rem_bytes, rx_bytes);
	rx_buf += (mas->cur_xfer->len - mas->rx_rem_bytes);
	while (i < rx_bytes) {
		u32 fifo_word = 0;
		u8 *fifo_byte;
		int bytes_per_fifo = fifo_width;
		int read_bytes = 0;
		int j;

		if ((mas->tx_fifo_width % mas->cur_word_len))
			bytes_per_fifo =
				(mas->cur_word_len / BITS_PER_BYTE) + 1;
		read_bytes = min_t(int, rx_bytes - i, bytes_per_fifo);
		ioread32_rep(se->base + SE_GENI_RX_FIFOn, &fifo_word, 1);
		fifo_byte = (u8 *)&fifo_word;
		for (j = 0; j < read_bytes; j++)
			rx_buf[i++] = fifo_byte[j];
	}
	mas->rx_rem_bytes -= rx_bytes;
}

static irqreturn_t geni_spi_isr(int irq, void *dev)
{
	struct spi_geni_master *mas = dev;
	struct geni_se *se = &mas->se;
	u32 m_irq = 0;
	irqreturn_t ret = IRQ_HANDLED;
	unsigned long flags;

	spin_lock_irqsave(&mas->lock, flags);
	if (mas->suspended) {
		ret = IRQ_NONE;
		goto exit_geni_spi_irq;
	}
	m_irq = readl_relaxed(se->base + SE_GENI_M_IRQ_STATUS);
	if ((m_irq & M_RX_FIFO_WATERMARK_EN) || (m_irq & M_RX_FIFO_LAST_EN))
		geni_spi_handle_rx(mas);

	if ((m_irq & M_TX_FIFO_WATERMARK_EN))
		geni_spi_handle_tx(mas);

	if ((m_irq & M_CMD_DONE_EN) || (m_irq & M_CMD_CANCEL_EN) ||
		(m_irq & M_CMD_ABORT_EN)) {
		complete(&mas->xfer_done);
		/*
		 * If this happens, then a CMD_DONE came before all the Tx
		 * buffer bytes were sent out. This is unusual, log this
		 * condition and disable the WM interrupt to prevent the
		 * system from stalling due an interrupt storm.
		 * If this happens when all Rx bytes haven't been received, log
		 * the condition.
		 * The only known time this can happen is if bits_per_word != 8
		 * and some registers that expect xfer lengths in num spi_words
		 * weren't written correctly.
		 */
		if (mas->tx_rem_bytes) {
			writel_relaxed(0, se->base + SE_GENI_TX_WATERMARK_REG);
			dev_err(mas->dev,
				"%s:Premature Done.tx_rem%d bpw%d\n",
				__func__, mas->tx_rem_bytes, mas->cur_word_len);
		}
		if (mas->rx_rem_bytes)
			dev_err(mas->dev,
				"%s:Premature Done.rx_rem%d bpw%d\n",
				__func__, mas->rx_rem_bytes, mas->cur_word_len);
	}
exit_geni_spi_irq:
	writel_relaxed(m_irq, se->base + SE_GENI_M_IRQ_CLEAR);
	spin_unlock_irqrestore(&mas->lock, flags);
	return ret;
}

static int spi_geni_probe(struct platform_device *pdev)
{
	int ret;
	struct spi_master *spi;
	struct spi_geni_master *spi_geni;
	struct resource *res;
	struct geni_se *se;

	spi = spi_alloc_master(&pdev->dev, sizeof(struct spi_geni_master));
	if (!spi) {
		ret = -ENOMEM;
		dev_err(&pdev->dev, "Failed to alloc spi struct\n");
		goto spi_geni_probe_err;
	}

	platform_set_drvdata(pdev, spi);
	spi_geni = spi_master_get_devdata(spi);
	spi_geni->dev = &pdev->dev;
	spi_geni->se.dev = &pdev->dev;
	spi_geni->se.wrapper = dev_get_drvdata(pdev->dev.parent);
	se = &spi_geni->se;

	spi->bus_num = of_alias_get_id(pdev->dev.of_node, "spi");
	spi->dev.of_node = pdev->dev.of_node;
	spi_geni->se.clk = devm_clk_get(&pdev->dev, "se");
	if (IS_ERR(spi_geni->se.clk)) {
		ret = PTR_ERR(spi_geni->se.clk);
		dev_err(&pdev->dev, "Err getting SE Core clk %d\n", ret);
		goto spi_geni_probe_err;
	}

	if (of_property_read_u32(pdev->dev.of_node, "spi-max-frequency",
				&spi->max_speed_hz)) {
		dev_err(&pdev->dev, "Max frequency not specified.\n");
		ret = -ENXIO;
		goto spi_geni_probe_err;
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	se->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(se->base)) {
		ret = -ENOMEM;
		dev_err(&pdev->dev, "Err IO mapping iomem\n");
		goto spi_geni_probe_err;
	}

	spi_geni->irq = platform_get_irq(pdev, 0);
	if (spi_geni->irq < 0) {
		dev_err(&pdev->dev, "Err getting IRQ\n");
		ret = spi_geni->irq;
		goto spi_geni_probe_unmap;
	}

	spi_geni->suspended = 1;
	spi->mode_bits = SPI_CPOL | SPI_CPHA | SPI_LOOP | SPI_CS_HIGH;
	spi->bits_per_word_mask = SPI_BPW_RANGE_MASK(4, 32);
	spi->num_chipselect = SPI_NUM_CHIPSELECT;
	spi->prepare_transfer_hardware = spi_geni_prepare_transfer_hardware;
	spi->prepare_message = spi_geni_prepare_message;
	spi->unprepare_message = spi_geni_unprepare_message;
	spi->transfer_one = spi_geni_transfer_one;
	spi->unprepare_transfer_hardware
			= spi_geni_unprepare_transfer_hardware;
	spi->auto_runtime_pm = false;

	init_completion(&spi_geni->xfer_done);
	spin_lock_init(&spi_geni->lock);
	pm_runtime_enable(&pdev->dev);
	ret = spi_register_master(spi);
	if (ret) {
		dev_err(&pdev->dev, "Failed to register SPI master\n");
		goto spi_geni_probe_unmap;
	}
	dev_dbg(&pdev->dev, "%s Probed\n", __func__);
	return ret;
spi_geni_probe_unmap:
	devm_iounmap(&pdev->dev, se->base);
spi_geni_probe_err:
	spi_master_put(spi);
	return ret;
}

static int spi_geni_remove(struct platform_device *pdev)
{
	struct spi_master *master = platform_get_drvdata(pdev);
	struct spi_geni_master *spi_geni = spi_master_get_devdata(master);

	spi_unregister_master(master);
	geni_se_resources_off(&spi_geni->se);
	pm_runtime_put_noidle(&pdev->dev);
	pm_runtime_disable(&pdev->dev);
	return 0;
}

static int __maybe_unused spi_geni_runtime_suspend(struct device *dev)
{
	int ret;
	struct spi_master *spi = get_spi_master(dev);
	struct spi_geni_master *spi_geni = spi_master_get_devdata(spi);

	if (spi_geni->suspended)
		return 0;

	ret = geni_se_resources_off(&spi_geni->se);
	if (!ret)
		spi_geni->suspended = 1;

	return ret;
}

static int __maybe_unused spi_geni_runtime_resume(struct device *dev)
{
	int ret;
	struct spi_master *spi = get_spi_master(dev);
	struct spi_geni_master *spi_geni = spi_master_get_devdata(spi);

	if (!spi_geni->suspended)
		return 0;

	ret = geni_se_resources_on(&spi_geni->se);
	if (!ret)
		spi_geni->suspended = 0;

	return ret;
}

static int __maybe_unused spi_geni_suspend(struct device *dev)
{
	return spi_geni_runtime_suspend(dev);
}

static int __maybe_unused spi_geni_resume(struct device *dev)
{
	return spi_geni_runtime_resume(dev);
}

static const struct dev_pm_ops spi_geni_pm_ops = {
	SET_RUNTIME_PM_OPS(spi_geni_runtime_suspend,
					spi_geni_runtime_resume, NULL)
	SET_SYSTEM_SLEEP_PM_OPS(spi_geni_suspend, spi_geni_resume)
};

static const struct of_device_id spi_geni_dt_match[] = {
	{ .compatible = "qcom,geni-spi" },
	{}
};

static struct platform_driver spi_geni_driver = {
	.probe  = spi_geni_probe,
	.remove = spi_geni_remove,
	.driver = {
		.name = "geni_spi",
		.pm = &spi_geni_pm_ops,
		.of_match_table = spi_geni_dt_match,
	},
};
module_platform_driver(spi_geni_driver);

MODULE_DESCRIPTION("SPI driver for GENI based QUP cores");
MODULE_LICENSE("GPL v2");
