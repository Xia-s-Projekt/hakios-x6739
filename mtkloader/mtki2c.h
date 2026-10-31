/*
 * MediaTek MT6893 I2C0 and FocalTech FT3681 driver for mtkloader.
 */
#ifndef MTKI2C_H
#define MTKI2C_H

#include <stdint.h>
#include "mt6893.h"
#include "mtkdbg.h"

extern void dcache_clean_range(uint64_t va, uint64_t len);
extern void out(const char *s);
extern void out_hex(uint64_t v);
extern void out_dec(uint64_t v);

static inline uint8_t rd8(uint64_t a) { return *(volatile uint8_t *)a; }
static inline void wr8(uint64_t a, uint8_t v) { *(volatile uint8_t *)a = v; }
static inline uint16_t rd16(uint64_t a) { return *(volatile uint16_t *)a; }
static inline void wr16(uint64_t a, uint16_t v) { *(volatile uint16_t *)a = v; }

#define I2C0_BASE                 0x11015000ULL
#define I2C0_DMA_BASE             0x11000080ULL
#define IMP_IIC_WRAP_C_BASE       0x11008000ULL
#define FTS_SLAVE_ADDR            0x38u

#define I2C_REG_PORT              0x00u
#define I2C_REG_SLAVE_ADDR        0x04u
#define I2C_REG_SLAVE_ADDR_V3     0x94u
#define I2C_REG_INTR_MASK         0x08u
#define I2C_REG_INTR_STAT         0x0cu
#define I2C_REG_CONTROL           0x10u
#define I2C_REG_TRANSFER_LEN      0x14u
#define I2C_REG_TRANSAC_LEN       0x18u
#define I2C_REG_DELAY_LEN         0x1cu
#define I2C_REG_TIMING            0x20u
#define I2C_REG_START             0x24u
#define I2C_REG_EXT_CONF          0x28u
#define I2C_REG_LTIMING           0x2cu
#define I2C_REG_HS                0x30u
#define I2C_REG_IO_CONFIG         0x34u
#define I2C_REG_FIFO_ADDR_CLR     0x38u
#define I2C_REG_TRANSFER_LEN_AUX  0x44u
#define I2C_REG_CLOCK_DIV         0x48u
#define I2C_REG_SOFTRESET         0x50u
#define I2C_REG_DEBUGSTAT         0xe4u
#define I2C_REG_DEBUGCTRL         0xe8u
#define I2C_REG_FIFO_STAT         0xf4u
#define I2C_REG_DCM_EN            0xf88u

#define APDMA_REG_INT_FLAG        0x00u
#define APDMA_REG_INT_EN          0x04u
#define APDMA_REG_EN              0x08u
#define APDMA_REG_RST             0x0cu
#define APDMA_REG_CON             0x18u
#define APDMA_REG_TX_MEM_ADDR     0x1cu
#define APDMA_REG_RX_MEM_ADDR     0x20u
#define APDMA_REG_TX_LEN          0x24u
#define APDMA_REG_RX_LEN          0x28u
#define APDMA_REG_TX_4G_MODE      0x54u
#define APDMA_REG_RX_4G_MODE      0x58u

#define I2C_CTL_RS                (1u << 1)
#define I2C_CTL_DMA_EN            (1u << 2)
#define I2C_CTL_CLK_EXT_EN        (1u << 3)
#define I2C_CTL_DIR_CHANGE        (1u << 4)
#define I2C_CTL_ACKERR_DET_EN     (1u << 5)
#define I2C_CTL_DMAACK_EN         (1u << 8)
#define I2C_CTL_ASYNC_MODE        (1u << 9)

#define I2C_STAT_TRANSAC_COMP     (1u << 0)
#define I2C_STAT_ACKERR           (1u << 1)
#define I2C_STAT_HS_NACKERR       (1u << 2)
#define I2C_STAT_ARB_LOST         (1u << 3)
#define I2C_STAT_RS_TRANSFER      (1u << 4)

#define I2C_START_TRIG            (1u << 0)
#define I2C_DEBUGCTRL_DMAACK_EN   (1u << 3)
#define I2C_DEBUGCTRL_RELIABILITY (1u << 4)

#define I2C_FS_START_CON          0x1800u
#define I2C_ST_START_CON          0x0800u

#define I2C_DMA_CON_TX            0x0000u
#define I2C_DMA_CON_RX            0x0001u
#define I2C_DMA_ASYNC_MODE        0x0004u
#define I2C_DMA_SKIP_CONFIG       0x0010u
#define I2C_DMA_DIR_CHANGE        0x0200u

#define I2C_DMA_START_EN          0x0001u
#define I2C_DMA_WARM_RST          0x0001u
#define I2C_DMA_HARD_RST          0x0002u
#define I2C_DMA_HANDSHAKE_RST     0x0004u
#define I2C_HANDSHAKE_RST         0x0020u
#define I2C_SOFT_RST              0x0001u

#define FTS_REG_DEV_MODE          0x00u
#define FTS_REG_GEST_ID           0x01u
#define FTS_REG_TD_STATUS         0x02u
#define FTS_REG_POINTS            0x03u
#define FTS_REG_CHIP_ID_H         0xa3u
#define FTS_REG_CHIP_ID_L         0x9fu
#define FTS_REG_FW_VER            0xa6u
#define FTS_REG_VENDOR_ID         0xa8u

static uint8_t i2c0_tx_buf[64] __attribute__((aligned(64)));
static uint8_t i2c0_rx_buf[64] __attribute__((aligned(64)));

static void fts_hw_reset(void)
{
	/* FT3681 reset is GPIO 20 on MT6893 pinctrl */
	uint32_t mode = rd32(GPIO_BASE + 0x320);
	mode &= ~(0xfu << 16);
	wr32(GPIO_BASE + 0x320, mode);

	wr32(GPIO_BASE + 0x004, 1u << 20);
	wr32(GPIO_BASE + 0x108, 1u << 20);
	dbg_delay_us(10000);
	wr32(GPIO_BASE + 0x104, 1u << 20);
	dbg_delay_us(200000);
}

/* ---- Software Open-Drain Bitbang on GPIO 204 (SCL) and 205 (SDA) ---- */

static inline void bb_scl_hi(void) { wr32(GPIO_BASE + 0x068, 1u << 12); }
static inline void bb_scl_lo(void) { wr32(GPIO_BASE + 0x064, 1u << 12); }
static inline void bb_sda_hi(void) { wr32(GPIO_BASE + 0x068, 1u << 13); }
static inline void bb_sda_lo(void) { wr32(GPIO_BASE + 0x064, 1u << 13); }
static inline int bb_scl_rd(void) { return (rd32(GPIO_BASE + 0x260) >> 12) & 1; }
static inline int bb_sda_rd(void) { return (rd32(GPIO_BASE + 0x260) >> 13) & 1; }

static void i2c_bitbang_init(void)
{
	/* Configure pins 204 and 205 as GPIO mode 0 */
	uint32_t mode = rd32(GPIO_BASE + 0x490);
	mode &= ~((0xfu << 16) | (0xfu << 20));
	wr32(GPIO_BASE + 0x490, mode);

	/* Data out is kept 0 so setting DIR drives low and clearing DIR floats high */
	wr32(GPIO_BASE + 0x168, (1u << 12) | (1u << 13));
	bb_scl_hi();
	bb_sda_hi();
	dbg_delay_us(10);
}

static void i2c_bb_start(void)
{
	bb_sda_hi();
	bb_scl_hi();
	dbg_delay_us(5);
	bb_sda_lo();
	dbg_delay_us(5);
	bb_scl_lo();
	dbg_delay_us(5);
}

static void i2c_bb_stop(void)
{
	bb_sda_lo();
	bb_scl_lo();
	dbg_delay_us(5);
	bb_scl_hi();
	dbg_delay_us(5);
	bb_sda_hi();
	dbg_delay_us(5);
}

static int i2c_bb_write_byte(uint8_t byte)
{
	for (int i = 7; i >= 0; i--) {
		if ((byte >> i) & 1) {
			bb_sda_hi();
		} else {
			bb_sda_lo();
		}
		dbg_delay_us(5);
		bb_scl_hi();
		dbg_delay_us(5);
		bb_scl_lo();
		dbg_delay_us(5);
	}
	bb_sda_hi();
	dbg_delay_us(5);
	bb_scl_hi();
	dbg_delay_us(5);
	int ack = (bb_sda_rd() == 0);
	bb_scl_lo();
	dbg_delay_us(5);
	return ack;
}

static uint8_t i2c_bb_read_byte(int ack)
{
	uint8_t byte = 0;
	bb_sda_hi();
	for (int i = 7; i >= 0; i--) {
		dbg_delay_us(5);
		bb_scl_hi();
		dbg_delay_us(5);
		if (bb_sda_rd()) {
			byte |= (1u << i);
		}
		bb_scl_lo();
		dbg_delay_us(5);
	}
	if (ack) {
		bb_sda_lo();
	} else {
		bb_sda_hi();
	}
	dbg_delay_us(5);
	bb_scl_hi();
	dbg_delay_us(5);
	bb_scl_lo();
	bb_sda_hi();
	dbg_delay_us(5);
	return byte;
}

static int i2c_bb_read_reg(uint8_t slave_addr, uint8_t reg, uint8_t *val)
{
	i2c_bb_start();
	if (!i2c_bb_write_byte((uint8_t)(slave_addr << 1))) {
		i2c_bb_stop();
		return -1;
	}
	if (!i2c_bb_write_byte(reg)) {
		i2c_bb_stop();
		return -2;
	}
	i2c_bb_start();
	if (!i2c_bb_write_byte((uint8_t)((slave_addr << 1) | 1))) {
		i2c_bb_stop();
		return -3;
	}
	*val = i2c_bb_read_byte(0);
	i2c_bb_stop();
	return 0;
}

/* ---- Hardware I2C0 Controller on MT6893 ---- */

static void i2c_hw_pinmux(void)
{
	/* Assign pins 204 and 205 to Mode 1 (SCL0 and SDA0) */
	uint32_t mode = rd32(GPIO_BASE + 0x490);
	mode &= ~((0xfu << 16) | (0xfu << 20));
	mode |= (1u << 16) | (1u << 20);
	wr32(GPIO_BASE + 0x490, mode);
}

static void mtk_i2c0_ungate_clocks(void)
{
	/* Clear all 6 infracfg gate registers */
	static const uint32_t infra_gate_clr[] = { 0x84, 0x8c, 0xa8, 0xc4, 0xd4, 0xe4 };
	for (unsigned int i = 0; i < sizeof(infra_gate_clr)/sizeof(infra_gate_clr[0]); i++) {
		wr32(INFRACFG_BASE + infra_gate_clr[i], 0xffffffffu);
	}

	wr32(IMP_IIC_WRAP_C_BASE + 0xe04, 0xffffffffu);
	wr32(TOPCKGEN_BASE + 0x0b8, 1u << 15);
	wr32(TOPCKGEN_BASE + 0x008, 1u << 10);
	__asm__ volatile("dsb sy");
}

static void mtk_i2c0_init_hw(void)
{
	mtk_i2c0_ungate_clocks();
	i2c_hw_pinmux();

	wr32(I2C0_DMA_BASE + APDMA_REG_RST, I2C_DMA_WARM_RST);
	dbg_delay_us(10);
	wr32(I2C0_DMA_BASE + APDMA_REG_RST, 0);
	dbg_delay_us(10);
	wr32(I2C0_DMA_BASE + APDMA_REG_RST, I2C_DMA_HANDSHAKE_RST | I2C_DMA_HARD_RST);
	wr16(I2C0_BASE + I2C_REG_SOFTRESET, I2C_HANDSHAKE_RST | I2C_SOFT_RST);
	dbg_delay_us(10);
	wr32(I2C0_DMA_BASE + APDMA_REG_RST, 0);
	wr16(I2C0_BASE + I2C_REG_SOFTRESET, 0);

	wr16(I2C0_BASE + I2C_REG_IO_CONFIG, 0x0003);
	wr16(I2C0_BASE + I2C_REG_DCM_EN, 0);
	wr16(I2C0_BASE + I2C_REG_DELAY_LEN, 2);

	/* 400kHz fast mode timings for MT6893 */
	wr16(I2C0_BASE + I2C_REG_CLOCK_DIV, 0);
	wr16(I2C0_BASE + I2C_REG_TIMING, (4u << 8) | 21u);
	wr16(I2C0_BASE + I2C_REG_LTIMING, (4u << 6) | 21u);
	wr16(I2C0_BASE + I2C_REG_EXT_CONF, I2C_FS_START_CON);

	uint16_t ctl = I2C_CTL_ACKERR_DET_EN | I2C_CTL_CLK_EXT_EN | I2C_CTL_DMA_EN | I2C_CTL_DMAACK_EN | I2C_CTL_ASYNC_MODE;
	wr16(I2C0_BASE + I2C_REG_CONTROL, ctl);
	wr16(I2C0_BASE + I2C_REG_DEBUGCTRL, I2C_DEBUGCTRL_RELIABILITY | I2C_DEBUGCTRL_DMAACK_EN);
}

static int mtk_i2c0_write_read_dma(uint8_t slave_addr, const uint8_t *tx, uint32_t tx_len, uint8_t *rx, uint32_t rx_len)
{
	if (tx_len > sizeof(i2c0_tx_buf) || rx_len > sizeof(i2c0_rx_buf)) {
		return -1;
	}

	for (uint32_t i = 0; i < tx_len; i++) {
		i2c0_tx_buf[i] = tx[i];
	}

	wr32(I2C0_DMA_BASE + APDMA_REG_RST, I2C_DMA_HARD_RST);
	dbg_delay_us(10);
	wr32(I2C0_DMA_BASE + APDMA_REG_RST, 0);
	wr16(I2C0_BASE + I2C_REG_FIFO_ADDR_CLR, 1);

	wr16(I2C0_BASE + I2C_REG_INTR_STAT, 0xffffu);
	wr16(I2C0_BASE + I2C_REG_INTR_MASK, 0xffffu);

	uint16_t ctl = I2C_CTL_DIR_CHANGE | I2C_CTL_RS | I2C_CTL_DMA_EN | I2C_CTL_CLK_EXT_EN |
	               I2C_CTL_ACKERR_DET_EN | I2C_CTL_DMAACK_EN | I2C_CTL_ASYNC_MODE;
	wr16(I2C0_BASE + I2C_REG_CONTROL, ctl);
	wr16(I2C0_BASE + I2C_REG_DEBUGCTRL, I2C_DEBUGCTRL_RELIABILITY | I2C_DEBUGCTRL_DMAACK_EN);

	wr16(I2C0_BASE + I2C_REG_SLAVE_ADDR, (uint16_t)slave_addr << 1);
	wr16(I2C0_BASE + I2C_REG_SLAVE_ADDR_V3, (uint16_t)slave_addr << 1);
	wr16(I2C0_BASE + I2C_REG_EXT_CONF, I2C_FS_START_CON);

	wr16(I2C0_BASE + I2C_REG_TRANSFER_LEN, (uint16_t)tx_len);
	wr16(I2C0_BASE + I2C_REG_TRANSFER_LEN_AUX, (uint16_t)rx_len);
	wr16(I2C0_BASE + I2C_REG_TRANSAC_LEN, 2);

	uint32_t dma_sync = I2C_DMA_SKIP_CONFIG | I2C_DMA_ASYNC_MODE | I2C_DMA_DIR_CHANGE;
	wr32(I2C0_DMA_BASE + APDMA_REG_INT_FLAG, 0);
	wr32(I2C0_DMA_BASE + APDMA_REG_CON, dma_sync);
	wr32(I2C0_DMA_BASE + APDMA_REG_TX_MEM_ADDR, (uint32_t)(uintptr_t)i2c0_tx_buf);
	wr32(I2C0_DMA_BASE + APDMA_REG_TX_LEN, tx_len);
	wr32(I2C0_DMA_BASE + APDMA_REG_RX_MEM_ADDR, (uint32_t)(uintptr_t)i2c0_rx_buf);
	wr32(I2C0_DMA_BASE + APDMA_REG_RX_LEN, rx_len);
	wr32(I2C0_DMA_BASE + APDMA_REG_TX_4G_MODE, 0);
	wr32(I2C0_DMA_BASE + APDMA_REG_RX_4G_MODE, 0);
	wr32(I2C0_DMA_BASE + APDMA_REG_EN, I2C_DMA_START_EN);

	dcache_clean_range((uint64_t)(uintptr_t)i2c0_tx_buf, tx_len);
	dcache_clean_range((uint64_t)(uintptr_t)i2c0_rx_buf, rx_len);

	wr16(I2C0_BASE + I2C_REG_START, I2C_START_TRIG);

	uint32_t timeout = 50000;
	while (timeout--) {
		uint16_t stat = rd16(I2C0_BASE + I2C_REG_INTR_STAT);
		if (stat & (I2C_STAT_ACKERR | I2C_STAT_HS_NACKERR)) {
			out(" [DMA-NACK stat=");
			out_hex(stat);
			out("] ");
			mtk_i2c0_init_hw();
			return -2;
		}
		if (stat & I2C_STAT_TRANSAC_COMP) {
			dcache_clean_range((uint64_t)(uintptr_t)i2c0_rx_buf, rx_len);
			for (uint32_t i = 0; i < rx_len; i++) {
				rx[i] = i2c0_rx_buf[i];
			}
			return 0;
		}
		dbg_delay_us(1);
	}

	out(" [DMA-TMO stat=");
	out_hex(rd16(I2C0_BASE + I2C_REG_INTR_STAT));
	out(" debug=");
	out_hex(rd16(I2C0_BASE + I2C_REG_DEBUGSTAT));
	out("] ");
	mtk_i2c0_init_hw();
	return -3;
}

static int fts_read_byte(uint8_t reg, uint8_t *val)
{
	int ret = mtk_i2c0_write_read_dma(FTS_SLAVE_ADDR, &reg, 1, val, 1);
	if (ret == 0) {
		return 0;
	}
	/* Fall back to bitbang */
	i2c_bitbang_init();
	ret = i2c_bb_read_reg(FTS_SLAVE_ADDR, reg, val);
	i2c_hw_pinmux();
	return ret;
}

#endif /* MTKI2C_H */
