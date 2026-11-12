/*
 * MediaTek MT6893 I2C0 and FocalTech FT3681 driver for mtkloader.
 *
 * The controller follows MediaTek's 6.1 i2c-mt65xx.c with mt6873_compat (regs v2, slave address
 * at 0x04, auto_restart, apdma_sync). Clocks: clk-mt6885.c / clk-mt6885-i2c.c. Pins:
 * pinctrl-mt6885.c. Every number is in docs/touchscreen-ft3681.md with its source.
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
#define IOCFG_LT_BASE             0x11f20000ULL
#define FTS_SLAVE_ADDR            0x38u

#define I2C_REG_PORT              0x00u
#define I2C_REG_SLAVE_ADDR        0x04u
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
#define I2C_REG_SDA_TIMING        0x3cu
#define I2C_REG_TRANSFER_LEN_AUX  0x44u
#define I2C_REG_CLOCK_DIV         0x48u
#define I2C_REG_SOFTRESET         0x50u
#define I2C_REG_SCL_MIS_COMP      0x90u
#define I2C_REG_DEBUGSTAT         0xe4u
#define I2C_REG_DEBUGCTRL         0xe8u
#define I2C_REG_FIFO_STAT         0xf4u

#define APDMA_REG_INT_FLAG        0x00u
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
#define I2C_STAT_CONFERR          (1u << 9)
#define I2C_STAT_ALL              (I2C_STAT_TRANSAC_COMP | I2C_STAT_ACKERR | I2C_STAT_HS_NACKERR | \
                                   I2C_STAT_ARB_LOST | I2C_STAT_RS_TRANSFER | I2C_STAT_CONFERR)

#define I2C_TRANSAC_START         (1u << 0)
#define I2C_RS_MUL_TRIG           (1u << 14)

#define I2C_FIFO_SIZE             8u

#define I2C_DMA_ASYNC_MODE        0x0004u
#define I2C_DMA_SKIP_CONFIG       0x0010u
#define I2C_DMA_DIR_CHANGE        0x0200u
#define I2C_DMA_START_EN          0x0001u
#define I2C_DMA_WARM_RST          0x0001u
#define I2C_DMA_HARD_RST          0x0002u
#define I2C_DMA_HANDSHAKE_RST     0x0004u
#define I2C_HANDSHAKE_RST         0x0020u
#define I2C_SOFT_RST              0x0001u

/* tools/i2c-timing.py: parent 124.8 MHz, 400 kHz, mt6873_compat */
#define I2C0_TIMING               0x0419u
#define I2C0_LTIMING              0x0123u
#define I2C0_EXT_CONF             0x5801u
#define I2C0_SDA_TIMING           0x100au

#define FTS_REG_DEV_MODE          0x00u
#define FTS_REG_GEST_ID           0x01u
#define FTS_REG_TD_STATUS         0x02u
#define FTS_REG_POINTS            0x03u
#define FTS_REG_CHIP_ID_H         0xa3u
#define FTS_REG_CHIP_ID_L         0x9fu
#define FTS_REG_FW_VER            0xa6u
#define FTS_REG_VENDOR_ID         0xa8u

/* The touch report and its decoding: mtkloader/fts_report.h. */
#include "fts_report.h"

/*
 * MT6885 Paris pinctrl: race_free_access is true.
 * Writing pinmux mode MUST go to MWR_OFFSET (0x0c) with bit 3 (0x8) set per nibble.
 */
#define MWR_OFFSET                0x0cu
#define I2C0_PIN_MODE_REG         0x490u
#define I2C0_PIN_BITS             ((1u << 12) | (1u << 13))

static uint8_t i2c0_tx_buf[64] __attribute__((aligned(64)));
static uint8_t i2c0_rx_buf[64] __attribute__((aligned(64)));

static void fts_reset_assert(void)
{
	/* GPIO20: mode 0x320 bitpos 16, MWR 0x32c, DIR 0x000 bit 20, DOUT 0x100 bit 20 */
	wr32(GPIO_BASE + 0x320 + MWR_OFFSET, (0x8u | 0u) << 16);

	wr32(GPIO_BASE + 0x004, 1u << 20);
	wr32(GPIO_BASE + 0x108, 1u << 20);
}

static void fts_reset_release(void)
{
	wr32(GPIO_BASE + 0x104, 1u << 20);
	dbg_delay_us(200000);
}

/*
 * The I2C0 pins: GPIO204 (SCL0) and GPIO205 (SDA0), in the IOCFG_LT block at 0x11f20000.
 *
 * Each field covers both pins in one register, with a bit per pin: 204 is the lower bit, 205 the
 * next one (pinctrl-mt6885.c, mt6885_pin_ies_range and the rest). A register holds its value at
 * +0x0, a "set" register at +0x4 and a "clear" register at +0x8.
 *
 * The settings are the ones the phone's own kernel has on these pins: the 203 ohm pull-ups
 * (rsel = 3, R0 and R1 both 1), the input buffers on, the pull-downs off, the schmitt triggers on,
 * and the drive strength at 7.
 */
static void i2c0_pin_pull_up(void)
{
	/* IES (0x0040): input enable, 204 at bit 15 and 205 at bit 16 */
	wr32(IOCFG_LT_BASE + 0x0044, (1u << 15) | (1u << 16));
	/* PD (0x0060): clear pull-down, 204 at bit 15 and 205 at bit 16 */
	wr32(IOCFG_LT_BASE + 0x0068, (1u << 15) | (1u << 16));
	/* PU (0x0080): set pull-up, 204 at bit 15 and 205 at bit 16 */
	wr32(IOCFG_LT_BASE + 0x0084, (1u << 15) | (1u << 16));
	/* SMT (0x00e0): schmitt trigger, 204 at bit 7 and 205 at bit 8 */
	wr32(IOCFG_LT_BASE + 0x00e4, (1u << 7) | (1u << 8));
	/* R0/R1 (0x00d0): rsel = 3, 204 at bits 1:0 and 205 at bits 3:2 */
	wr32(IOCFG_LT_BASE + 0x00d4, 0x0f);
	/* EH (0x0020): drive strength, 204 at bits 2:0 and 205 at bits 5:3 */
	wr32(IOCFG_LT_BASE + 0x0024, (7u << 0) | (7u << 3));
}

static void i2c0_pin_mode(uint32_t func)
{
	uint32_t val = ((0x8u | (func & 7u)) << 16) | ((0x8u | (func & 7u)) << 20);
	wr32(GPIO_BASE + I2C0_PIN_MODE_REG + MWR_OFFSET, val);
}

static void i2c0_dump_state(const char *tag)
{
	out("i2c0[");
	out(tag);
	out("]: mode=");
	out_hex((rd32(GPIO_BASE + I2C0_PIN_MODE_REG) >> 16) & 0xffu);
	out(" dir=");
	out_hex((rd32(GPIO_BASE + 0x060) >> 12) & 3u);
	out(" din=");
	out_hex((rd32(GPIO_BASE + 0x260) >> 12) & 3u);
	out(" pu=");
	out_hex((rd32(IOCFG_LT_BASE + 0x0080) >> 15) & 1u);
	out(" pd=");
	out_hex((rd32(IOCFG_LT_BASE + 0x0060) >> 15) & 1u);
	out(" rsel=");
	out_hex(rd32(IOCFG_LT_BASE + 0x00d0) & 0x0fu);
	out(" eh=");
	out_hex(rd32(IOCFG_LT_BASE + 0x0020) & 0x3fu);
	out(" smt=");
	out_hex((rd32(IOCFG_LT_BASE + 0x00e0) >> 7) & 1u);
	out(" cfg10=");
	out_hex(rd32(TOPCKGEN_BASE + 0x0b0));
	out(" ifrao0=");
	out_hex(rd32(INFRACFG_BASE + 0x090));
	out(" ifrao1=");
	out_hex(rd32(INFRACFG_BASE + 0x094));
	out(" ifrao2=");
	out_hex(rd32(INFRACFG_BASE + 0x0d8));
	out(" impc=");
	out_hex(rd32(IMP_IIC_WRAP_C_BASE + 0xe00));
	out(" rst_gpio=");
	out_hex((rd32(GPIO_BASE + 0x200) >> 20) & 3u);
	out("\n");
}

/* ---- Software open-drain bitbang on GPIO 204 (SCL) and 205 (SDA) ---- */

/* DOUT stays 0: DIR=1 (output) pulls the line low, DIR=0 (input) lets the pull-up raise it */
static inline void bb_scl_hi(void) { wr32(GPIO_BASE + 0x068, 1u << 12); }
static inline void bb_scl_lo(void) { wr32(GPIO_BASE + 0x064, 1u << 12); }
static inline void bb_sda_hi(void) { wr32(GPIO_BASE + 0x068, 1u << 13); }
static inline void bb_sda_lo(void) { wr32(GPIO_BASE + 0x064, 1u << 13); }
static inline int bb_scl_rd(void) { return (rd32(GPIO_BASE + 0x260) >> 12) & 1; }
static inline int bb_sda_rd(void) { return (rd32(GPIO_BASE + 0x260) >> 13) & 1; }

static int bb_scl_release(void)
{
	bb_scl_hi();
	for (uint32_t i = 0; i < 1000; i++) {
		if (bb_scl_rd()) {
			return 1;
		}
		dbg_delay_us(1);
	}
	return 0;
}

static void i2c_bitbang_init(void)
{
	i2c0_pin_mode(0);
	wr32(GPIO_BASE + 0x168, I2C0_PIN_BITS);
	bb_scl_hi();
	bb_sda_hi();
	dbg_delay_us(10);
}

static void i2c_bb_start(void)
{
	bb_sda_hi();
	bb_scl_release();
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
	bb_scl_release();
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
		bb_scl_release();
		dbg_delay_us(5);
		bb_scl_lo();
		dbg_delay_us(5);
	}
	bb_sda_hi();
	dbg_delay_us(5);
	bb_scl_release();
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
		bb_scl_release();
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
	bb_scl_release();
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

/* Which 7-bit addresses ACK on the bus; tells "nobody there" from "controller wrong". */
static void i2c_bb_scan(void)
{
	i2c_bitbang_init();
	uint32_t mode = (rd32(GPIO_BASE + I2C0_PIN_MODE_REG) >> 16) & 0xffu;
	int hi = (bb_scl_rd() << 1) | bb_sda_rd();
	bb_scl_lo();
	dbg_delay_us(5);
	int scl_lo = (bb_scl_rd() << 1) | bb_sda_rd();
	bb_scl_hi();
	bb_sda_lo();
	dbg_delay_us(5);
	int sda_lo = (bb_scl_rd() << 1) | bb_sda_rd();
	bb_sda_hi();
	dbg_delay_us(5);
	out("i2c0-bb: mode=");
	out_hex(mode);
	out(" hi=");
	out_dec(hi);
	out(" scl_lo=");
	out_dec(scl_lo);
	out(" sda_lo=");
	out_dec(sda_lo);

	/* Diagnostic: test DOUT driving while DIR=1 */
	wr32(GPIO_BASE + 0x064, I2C0_PIN_BITS); /* DIR=1 (output) */
	wr32(GPIO_BASE + 0x168, I2C0_PIN_BITS); /* DOUT=0 (drive low) */
	dbg_delay_us(5);
	out(" drv0_din=");
	out_hex((rd32(GPIO_BASE + 0x260) >> 12) & 3u);
	wr32(GPIO_BASE + 0x164, I2C0_PIN_BITS); /* DOUT=1 (drive high) */
	dbg_delay_us(5);
	out(" drv1_din=");
	out_hex((rd32(GPIO_BASE + 0x260) >> 12) & 3u);
	wr32(GPIO_BASE + 0x168, I2C0_PIN_BITS); /* DOUT=0 */
	wr32(GPIO_BASE + 0x068, I2C0_PIN_BITS); /* DIR=0 (release to input) */
	dbg_delay_us(5);

	out(" ack:");
	uint32_t found = 0;
	for (uint8_t a = 0x08; a < 0x78; a++) {
		i2c_bb_start();
		int ack = i2c_bb_write_byte((uint8_t)(a << 1));
		i2c_bb_stop();
		if (ack) {
			out(" ");
			out_hex(a);
			found++;
		}
	}
	if (!found) {
		out(" none");
	}
	out("\n");
}

/* ---- Hardware I2C0 controller ---- */

static void i2c_hw_pinmux(void)
{
	i2c0_pin_mode(1);
}

static void mtk_i2c0_ungate_clocks(void)
{
	/* i2c_sel: field CLK_CFG_10[9:8] <- 2 (univpll_d5_d4, 124.8 MHz), pdn bit 15 cleared */
	wr32(TOPCKGEN_BASE + 0x0b8, 3u << 8);
	wr32(TOPCKGEN_BASE + 0x0b4, 2u << 8);
	wr32(TOPCKGEN_BASE + 0x0b8, 1u << 15);
	wr32(TOPCKGEN_BASE + 0x008, 1u << 10);

	wr32(IMP_IIC_WRAP_C_BASE + 0xe04, 1u << 0);
	wr32(INFRACFG_BASE + 0x084, 1u << 11);
	wr32(INFRACFG_BASE + 0x08c, 1u << 18);
	wr32(INFRACFG_BASE + 0x0d4, 1u << 31);
	__asm__ volatile("dsb sy" ::: "memory");
	dbg_delay_us(10);
}

static void mtk_i2c0_init_hw(void)
{
	wr16(I2C0_BASE + I2C_REG_START, 0);
	wr16(I2C0_BASE + I2C_REG_INTR_STAT, rd16(I2C0_BASE + I2C_REG_INTR_STAT));

	wr32(I2C0_DMA_BASE + APDMA_REG_RST, I2C_DMA_WARM_RST);
	dbg_delay_us(2);
	wr32(I2C0_DMA_BASE + APDMA_REG_RST, 0);
	dbg_delay_us(2);
	wr32(I2C0_DMA_BASE + APDMA_REG_RST, I2C_DMA_HANDSHAKE_RST | I2C_DMA_HARD_RST);
	wr16(I2C0_BASE + I2C_REG_SOFTRESET, I2C_HANDSHAKE_RST | I2C_SOFT_RST);
	dbg_delay_us(2);
	wr32(I2C0_DMA_BASE + APDMA_REG_RST, 0);
	wr16(I2C0_BASE + I2C_REG_SOFTRESET, 0);

	wr16(I2C0_BASE + I2C_REG_IO_CONFIG, 0x0003);
	wr16(I2C0_BASE + I2C_REG_TIMING, I2C0_TIMING);
	wr16(I2C0_BASE + I2C_REG_HS, 0);
	wr16(I2C0_BASE + I2C_REG_LTIMING, I2C0_LTIMING);
	wr16(I2C0_BASE + I2C_REG_EXT_CONF, I2C0_EXT_CONF);
	wr16(I2C0_BASE + I2C_REG_CLOCK_DIV, 0);
	wr16(I2C0_BASE + I2C_REG_SCL_MIS_COMP, 0);
	wr16(I2C0_BASE + I2C_REG_SDA_TIMING, I2C0_SDA_TIMING);
	wr16(I2C0_BASE + I2C_REG_TIMING, I2C0_TIMING);
	wr16(I2C0_BASE + I2C_REG_HS, 0);
	wr16(I2C0_BASE + I2C_REG_LTIMING, I2C0_LTIMING);

	wr16(I2C0_BASE + I2C_REG_CONTROL, I2C_CTL_ACKERR_DET_EN | I2C_CTL_DMA_EN |
	     I2C_CTL_DMAACK_EN | I2C_CTL_ASYNC_MODE);
	wr16(I2C0_BASE + I2C_REG_DELAY_LEN, 2);
}

static void i2c0_report_fail(const char *what, uint16_t stat)
{
	out(" [");
	out(what);
	out(" stat=");
	out_hex(stat);
	out(" debug=");
	out_hex(rd16(I2C0_BASE + I2C_REG_DEBUGSTAT));
	out(" fifo=");
	out_hex(rd16(I2C0_BASE + I2C_REG_FIFO_STAT));
	out(" dma_fsm=");
	out_hex(rd16(I2C0_BASE + 0xec));
	out(" din=");
	out_hex((rd32(GPIO_BASE + 0x260) >> 12) & 3u);
	out("] ");
}

/* Poll until the transfer completes or fails; the loader runs with the I2C IRQ unrouted. */
static int i2c0_wait(void)
{
	uint16_t seen = 0;
	uint32_t rs_polls = 0;
	for (uint32_t t = 0; t < 50000; t++) {
		uint16_t stat = rd16(I2C0_BASE + I2C_REG_INTR_STAT);
		if (stat) {
			wr16(I2C0_BASE + I2C_REG_INTR_STAT, stat);
			seen |= stat;
		}
		if (seen & (I2C_STAT_ACKERR | I2C_STAT_HS_NACKERR | I2C_STAT_ARB_LOST)) {
			i2c0_report_fail("NACK", seen);
			mtk_i2c0_init_hw();
			return -2;
		}
		if (seen & I2C_STAT_TRANSAC_COMP) {
			return 0;
		}
		/* the 6.1 IRQ handler also completes on RS_TRANSFER under auto_restart */
		if ((seen & I2C_STAT_RS_TRANSFER) && ++rs_polls > 2000) {
			return 0;
		}
		dbg_delay_us(1);
	}
	i2c0_report_fail("TMO", seen);
	mtk_i2c0_init_hw();
	return -3;
}

static void i2c0_prepare(uint8_t slave_addr, uint32_t tx_len, uint32_t rx_len, int dma)
{
	uint16_t ctl = rd16(I2C0_BASE + I2C_REG_CONTROL) &
		~(I2C_CTL_DIR_CHANGE | I2C_CTL_RS | I2C_CTL_DMA_EN | I2C_CTL_DMAACK_EN | I2C_CTL_ASYNC_MODE | I2C_CTL_CLK_EXT_EN);
	ctl |= I2C_CTL_DIR_CHANGE | I2C_CTL_RS;
	if (dma) {
		ctl |= I2C_CTL_DMA_EN | I2C_CTL_DMAACK_EN | I2C_CTL_ASYNC_MODE;
	}
	wr16(I2C0_BASE + I2C_REG_CONTROL, ctl);
	wr16(I2C0_BASE + I2C_REG_SLAVE_ADDR, (uint16_t)(slave_addr << 1));
	wr16(I2C0_BASE + I2C_REG_INTR_STAT, I2C_STAT_ALL);
	wr16(I2C0_BASE + I2C_REG_FIFO_ADDR_CLR, 1);
	wr16(I2C0_BASE + I2C_REG_INTR_MASK, 0);
	wr16(I2C0_BASE + I2C_REG_TRANSFER_LEN, (uint16_t)tx_len);
	wr16(I2C0_BASE + I2C_REG_TRANSFER_LEN_AUX, (uint16_t)rx_len);
	wr16(I2C0_BASE + I2C_REG_TRANSAC_LEN, 2);
}

static int mtk_i2c0_write_read_fifo(uint8_t slave_addr, const uint8_t *tx, uint32_t tx_len, uint8_t *rx, uint32_t rx_len)
{
	if (tx_len > I2C_FIFO_SIZE || rx_len > I2C_FIFO_SIZE) {
		return -1;
	}
	i2c0_prepare(slave_addr, tx_len, rx_len, 0);
	for (uint32_t i = 0; i < tx_len; i++) {
		wr8(I2C0_BASE + I2C_REG_PORT, tx[i]);
	}
	__asm__ volatile("dsb sy" ::: "memory");
	wr16(I2C0_BASE + I2C_REG_START, I2C_TRANSAC_START);

	int ret = i2c0_wait();
	if (ret) {
		return ret;
	}
	for (uint32_t i = 0; i < rx_len; i++) {
		rx[i] = rd8(I2C0_BASE + I2C_REG_PORT);
	}
	return 0;
}

static int mtk_i2c0_write_read_dma(uint8_t slave_addr, const uint8_t *tx, uint32_t tx_len, uint8_t *rx, uint32_t rx_len)
{
	if (tx_len > sizeof(i2c0_tx_buf) || rx_len > sizeof(i2c0_rx_buf)) {
		return -1;
	}
	for (uint32_t i = 0; i < tx_len; i++) {
		i2c0_tx_buf[i] = tx[i];
	}
	dcache_clean_range((uint64_t)(uintptr_t)i2c0_tx_buf, sizeof(i2c0_tx_buf));
	dcache_clean_range((uint64_t)(uintptr_t)i2c0_rx_buf, sizeof(i2c0_rx_buf));

	/* apdma_sync and a read after a write: warm-reset the DMA, as the 6.1 driver does */
	wr32(I2C0_DMA_BASE + APDMA_REG_RST, I2C_DMA_HANDSHAKE_RST | I2C_DMA_WARM_RST);
	for (uint32_t t = 0; t < 100 && (rd32(I2C0_DMA_BASE + APDMA_REG_RST) & I2C_DMA_WARM_RST); t++) {
		dbg_delay_us(1);
	}
	wr32(I2C0_DMA_BASE + APDMA_REG_RST, 0);
	wr16(I2C0_BASE + I2C_REG_SOFTRESET, I2C_HANDSHAKE_RST);
	wr16(I2C0_BASE + I2C_REG_SOFTRESET, 0);

	i2c0_prepare(slave_addr, tx_len, rx_len, 1);

	wr32(I2C0_DMA_BASE + APDMA_REG_INT_FLAG, 0);
	wr32(I2C0_DMA_BASE + APDMA_REG_CON, I2C_DMA_SKIP_CONFIG | I2C_DMA_ASYNC_MODE | I2C_DMA_DIR_CHANGE);
	wr32(I2C0_DMA_BASE + APDMA_REG_TX_4G_MODE, (uint32_t)((uint64_t)(uintptr_t)i2c0_tx_buf >> 32));
	wr32(I2C0_DMA_BASE + APDMA_REG_TX_MEM_ADDR, (uint32_t)(uintptr_t)i2c0_tx_buf);
	wr32(I2C0_DMA_BASE + APDMA_REG_TX_LEN, tx_len);
	wr32(I2C0_DMA_BASE + APDMA_REG_RX_4G_MODE, (uint32_t)((uint64_t)(uintptr_t)i2c0_rx_buf >> 32));
	wr32(I2C0_DMA_BASE + APDMA_REG_RX_MEM_ADDR, (uint32_t)(uintptr_t)i2c0_rx_buf);
	wr32(I2C0_DMA_BASE + APDMA_REG_RX_LEN, rx_len);
	__asm__ volatile("dsb sy" ::: "memory");
	wr32(I2C0_DMA_BASE + APDMA_REG_EN, I2C_DMA_START_EN);
	__asm__ volatile("dsb sy" ::: "memory");
	wr16(I2C0_BASE + I2C_REG_START, I2C_TRANSAC_START);

	int ret = i2c0_wait();
	if (ret) {
		return ret;
	}
	dcache_clean_range((uint64_t)(uintptr_t)i2c0_rx_buf, sizeof(i2c0_rx_buf));
	for (uint32_t i = 0; i < rx_len; i++) {
		rx[i] = i2c0_rx_buf[i];
	}
	return 0;
}

/* Same split as the 6.1 driver: FIFO up to its 8 bytes, DMA beyond. */
static int mtk_i2c0_write_read(uint8_t slave_addr, const uint8_t *tx, uint32_t tx_len, uint8_t *rx, uint32_t rx_len)
{
	if (tx_len > I2C_FIFO_SIZE || rx_len > I2C_FIFO_SIZE) {
		return mtk_i2c0_write_read_dma(slave_addr, tx, tx_len, rx, rx_len);
	}
	return mtk_i2c0_write_read_fifo(slave_addr, tx, tx_len, rx, rx_len);
}

static int fts_read_byte(uint8_t reg, uint8_t *val)
{
	int ret = mtk_i2c0_write_read(FTS_SLAVE_ADDR, &reg, 1, val, 1);
	if (ret == 0) {
		return 0;
	}
	i2c_bitbang_init();
	ret = i2c_bb_read_reg(FTS_SLAVE_ADDR, reg, val);
	i2c_hw_pinmux();
	return ret ? ret - 10 : 0;
}

#endif /* MTKI2C_H */
