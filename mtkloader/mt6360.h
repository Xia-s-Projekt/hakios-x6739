/*
 * MT6360 LDO2 ("vtp", the touch rail) on the X6739, driven from the loader.
 *
 * The phone's own touch driver enables the panel's supply with regulator_enable(vtouch); the
 * device tree points vtouch-supply at /soc/i2c@11f00000/mt6360_pmu@34/ldo/ldo2, so the rail is
 * MT6360 LDO2 and the MT6360 sits on I2C5 (0x11f00000), not on the touch bus.
 *
 * LDO2 in the phone's own regulator module (drivers/regulator/mt6360-regulator.c, the source of
 * the shipped mt6360-regulator.ko):
 *   - voltage selector register 0x15, 3.3 V is 0xd0 (ldo_volt_ranges1)
 *   - enable register 0x11, mask 0x40
 *   - the write is an SMBus block write: [address byte, reg, value, crc8, dummy], where the
 *     crc8 is over [address byte, reg, value] with the CRC-8 polynomial 0x07 (MSB first, init 0)
 *   - the read is [address byte | 1, reg] then two bytes back: value and crc8, the crc8 again
 *     over [address byte | 1, reg, value]
 *
 * The address byte is the LDO bank's slave id (0x64) shifted left by one: 0xc8.
 *
 * I2C5 itself: the same v2 controller as I2C0 (both are "mediatek,mt6873-i2c"), base 0x11f00000,
 * on the shared i2c_ck parent (i2c_sel, 124.8 MHz, the mux the loader already programs for
 * I2C0). Its clock gates are imp_iic_wrap_n (syscon 0x11f02000) and the IFRAO3 i2c5 gates.
 * Pins: GPIO202 = SCL5, GPIO203 = SDA5 (function 1), whose IOCFG registers are in the IOCFG_TM
 * block at 0x11f30000.
 */
#ifndef MT6360_H
#define MT6360_H

#include <stdint.h>
#include "mt6893.h"
#include "mtkdbg.h"
#include "mtki2c.h"

#define I2C5_BASE                 0x11f00000ULL
#define IMP_IIC_WRAP_N_BASE       0x11f02000ULL
#define IOCFG_TM_BASE             0x11f30000ULL

#define MT6360_LDO_SLAVE_ADDR     0x64u
#define MT6360_LDO2_VOUT_REG      0x15u
#define MT6360_LDO2_VOUT_3V3      0xd0u
#define MT6360_LDO2_EN_REG        0x11u
#define MT6360_LDO2_EN_MASK       0x40u

/* CRC-8, polynomial 0x07, most significant bit first, initial value 0 (the table the phone's
 * own module builds with crc8_populate_msb). */
static uint8_t mt6360_crc8(const uint8_t *data, uint32_t len)
{
	uint8_t crc = 0;
	for (uint32_t i = 0; i < len; i++) {
		crc ^= data[i];
		for (int b = 0; b < 8; b++) {
			crc = (crc & 0x80u) ? (uint8_t)((crc << 1) ^ 0x07u) : (uint8_t)(crc << 1);
		}
	}
	return crc;
}

/* ---- the I2C5 pins ---- */

/*
 * GPIO202 and GPIO203, in the IOCFG_TM block. The value register is at +0x0 of each field's
 * address, the "set" register at +0x4 and the "clear" register at +0x8.
 *
 * The pull-ups are the same 203 ohm ones the phone's kernel has (rsel = 3: R0 and R1 both 1).
 */
static void i2c5_pin_config(void)
{
	/* input enable: 202 at bit 3, 203 at bit 6 */
	wr32(IOCFG_TM_BASE + 0x0064, (1u << 3) | (1u << 6));
	/* pull-down off: 202 at bit 23, 203 at bit 26 */
	wr32(IOCFG_TM_BASE + 0x0078, (1u << 23) | (1u << 26));
	/* pull-up on: 202 at bit 23, 203 at bit 26 */
	wr32(IOCFG_TM_BASE + 0x0094, (1u << 23) | (1u << 26));
	/* schmitt trigger on, as the phone's kernel has it */
	wr32(IOCFG_TM_BASE + 0x00f4, (1u << 10) | (1u << 13));
	/* rsel = 3 (203 ohm): R0/R1 at bits 4,5 for 202 and 8,9 for 203 */
	wr32(IOCFG_TM_BASE + 0x00e4, (1u << 4) | (1u << 5) | (1u << 8) | (1u << 9));
	/* drive strength, one setting (the phone's kernel leaves these two at 1) */
	wr32(IOCFG_TM_BASE + 0x0034, (1u << 6) | (1u << 12));
}

/* GPIO202 = SCL5, GPIO203 = SDA5, function 1. The mode register is at GPIO_BASE + 0x490, four
 * bits per pin: 202 at bits 8..11, 203 at bits 12..15. A mode write goes to the MWR port at
 * +0x0c, with the top bit of each field meaning "modify this field". */
static void i2c5_pin_mode(void)
{
	wr32(GPIO_BASE + 0x490 + MWR_OFFSET, ((0x8u | 1u) << 8) | ((0x8u | 1u) << 12));
}

/* ---- the I2C5 controller ---- */

static void mtk_i2c5_ungate_clocks(void)
{
	/* i2c_sel: field CLK_CFG_10[9:8] <- 2 (univpll_d5_d4, 124.8 MHz), pdn bit 15 cleared. I2C5
	 * takes the same parent as I2C0 (i2c_ck), so the mux is programmed here as well. */
	wr32(TOPCKGEN_BASE + 0x0b8, 3u << 8);
	wr32(TOPCKGEN_BASE + 0x0b4, 2u << 8);
	wr32(TOPCKGEN_BASE + 0x0b8, 1u << 15);
	wr32(TOPCKGEN_BASE + 0x008, 1u << 10);

	/* imp_iic_wrap_n: the I2C5 gate is bit 0 of the clear register */
	wr32(IMP_IIC_WRAP_N_BASE + 0xe04, 1u << 0);
	/* the i2c5 gates in IFRAO3: i2c5 (18), its arbiter (19), the immediate-mode one (20) */
	wr32(INFRACFG_BASE + 0x0a8, (1u << 18) | (1u << 19) | (1u << 20));
	/* the DMA and the peripheral bus, as for I2C0 */
	wr32(INFRACFG_BASE + 0x08c, 1u << 18);
	wr32(INFRACFG_BASE + 0x0d4, 1u << 31);
	__asm__ volatile("dsb sy" ::: "memory");
	dbg_delay_us(10);
}

static void mtk_i2c5_init_hw(void)
{
	wr16(I2C5_BASE + I2C_REG_SOFTRESET, I2C_HANDSHAKE_RST | I2C_SOFT_RST);
	dbg_delay_us(2);
	wr16(I2C5_BASE + I2C_REG_SOFTRESET, 0);
	dbg_delay_us(2);

	wr16(I2C5_BASE + I2C_REG_IO_CONFIG, 0x0003);
	wr16(I2C5_BASE + I2C_REG_TIMING, I2C0_TIMING);
	wr16(I2C5_BASE + I2C_REG_HS, 0);
	wr16(I2C5_BASE + I2C_REG_LTIMING, I2C0_LTIMING);
	wr16(I2C5_BASE + I2C_REG_EXT_CONF, I2C0_EXT_CONF);
	wr16(I2C5_BASE + I2C_REG_CLOCK_DIV, 0);
	wr16(I2C5_BASE + I2C_REG_SCL_MIS_COMP, 0);
	wr16(I2C5_BASE + I2C_REG_SDA_TIMING, I2C0_SDA_TIMING);
	wr16(I2C5_BASE + I2C_REG_CONTROL, I2C_CTL_ACKERR_DET_EN);
	wr16(I2C5_BASE + I2C_REG_DELAY_LEN, 2);
}

static int i2c5_wait(void)
{
	uint16_t seen = 0;
	for (uint32_t t = 0; t < 50000; t++) {
		uint16_t stat = rd16(I2C5_BASE + I2C_REG_INTR_STAT);
		if (stat) {
			wr16(I2C5_BASE + I2C_REG_INTR_STAT, stat);
			seen |= stat;
		}
		if (seen & (I2C_STAT_ACKERR | I2C_STAT_HS_NACKERR | I2C_STAT_ARB_LOST)) {
			return -2;
		}
		if (seen & I2C_STAT_TRANSAC_COMP) {
			return 0;
		}
		if ((seen & I2C_STAT_RS_TRANSFER) && t > 2000) {
			return 0;
		}
		dbg_delay_us(1);
	}
	return -3;
}

/* One transaction: tx_len bytes out, then rx_len bytes in (rx_len 0 means write only). The
 * controller's second phase is the read, with a repeated start between the two. */
static int i2c5_xfer(uint8_t slave, const uint8_t *tx, uint32_t tx_len, uint8_t *rx, uint32_t rx_len)
{
	if (tx_len > I2C_FIFO_SIZE || rx_len > I2C_FIFO_SIZE) {
		return -1;
	}
	uint16_t ctl = rd16(I2C5_BASE + I2C_REG_CONTROL) &
		~(I2C_CTL_DIR_CHANGE | I2C_CTL_RS | I2C_CTL_DMA_EN | I2C_CTL_DMAACK_EN |
		  I2C_CTL_ASYNC_MODE | I2C_CTL_CLK_EXT_EN);
	if (rx_len) {
		ctl |= I2C_CTL_DIR_CHANGE | I2C_CTL_RS;
	}
	wr16(I2C5_BASE + I2C_REG_CONTROL, ctl);
	wr16(I2C5_BASE + I2C_REG_SLAVE_ADDR, (uint16_t)(slave << 1));
	wr16(I2C5_BASE + I2C_REG_INTR_STAT, I2C_STAT_ALL);
	wr16(I2C5_BASE + I2C_REG_FIFO_ADDR_CLR, 1);
	wr16(I2C5_BASE + I2C_REG_INTR_MASK, 0);
	wr16(I2C5_BASE + I2C_REG_TRANSFER_LEN, (uint16_t)tx_len);
	wr16(I2C5_BASE + I2C_REG_TRANSFER_LEN_AUX, (uint16_t)rx_len);
	wr16(I2C5_BASE + I2C_REG_TRANSAC_LEN, rx_len ? 2 : 1);

	for (uint32_t i = 0; i < tx_len; i++) {
		wr8(I2C5_BASE + I2C_REG_PORT, tx[i]);
	}
	__asm__ volatile("dsb sy" ::: "memory");
	wr16(I2C5_BASE + I2C_REG_START, I2C_TRANSAC_START);

	int ret = i2c5_wait();
	if (ret) {
		return ret;
	}
	for (uint32_t i = 0; i < rx_len; i++) {
		rx[i] = rd8(I2C5_BASE + I2C_REG_PORT);
	}
	return 0;
}

/* ---- the MT6360 LDO bank ---- */

static int mt6360_write_reg(uint8_t slave, uint8_t reg, uint8_t val)
{
	uint8_t crc_in[3];
	uint8_t payload[4];

	crc_in[0] = (uint8_t)((slave << 1) & 0xffu);
	crc_in[1] = reg & 0x3fu;
	crc_in[2] = val;

	payload[0] = reg & 0x3fu;
	payload[1] = val;
	payload[2] = mt6360_crc8(crc_in, 3);
	payload[3] = 0x00;	/* the dummy byte the module sends */

	return i2c5_xfer(slave, payload, sizeof(payload), (uint8_t *)0, 0);
}

static int mt6360_read_reg(uint8_t slave, uint8_t reg, uint8_t *val)
{
	uint8_t tx = reg & 0x3fu;
	uint8_t rx[2];
	uint8_t crc_in[3];

	int ret = i2c5_xfer(slave, &tx, 1, rx, sizeof(rx));
	if (ret) {
		return ret;
	}
	crc_in[0] = (uint8_t)(((slave << 1) & 0xffu) | 1u);
	crc_in[1] = reg & 0x3fu;
	crc_in[2] = rx[0];
	if (mt6360_crc8(crc_in, 3) != rx[1]) {
		return -4;
	}
	*val = rx[0];
	return 0;
}

#define MT6360_PMIC_SLAVE_ADDR   0x1au
#define MT6360_LDO_SLAVE_ADDR    0x64u

/* Read one register of one MT6360 bank and print it. */
static void mt6360_show(const char *what, uint8_t slave, uint8_t reg)
{
	uint8_t val = 0;
	int ret = mt6360_read_reg(slave, reg, &val);
	out("mt6360: ");
	out(what);
	out("=");
	if (ret == 0) {
		out_hex(val);
	} else {
		out("ERR");
		out_dec((uint32_t)-ret);
	}
	out("\n");
}

/*
 * The state of the PMU as the loader finds it: the bucks that feed the LDO, and the LDO itself.
 * LDO2 cannot source anything if its input buck is off, so this says more than the enable bit
 * alone.
 */
static void mt6360_dump_state(void)
{
	out("mt6360: PMU state (read at the loader's start)\n");
	mt6360_show("buck1_vout", MT6360_PMIC_SLAVE_ADDR, 0x10);
	mt6360_show("buck1_en", MT6360_PMIC_SLAVE_ADDR, 0x17);
	mt6360_show("buck2_vout", MT6360_PMIC_SLAVE_ADDR, 0x20);
	mt6360_show("buck2_en", MT6360_PMIC_SLAVE_ADDR, 0x27);
	mt6360_show("ldo6_en", MT6360_PMIC_SLAVE_ADDR, 0x37);
	mt6360_show("ldo7_en", MT6360_PMIC_SLAVE_ADDR, 0x31);
	mt6360_show("ldo1_en", MT6360_LDO_SLAVE_ADDR, 0x17);
	mt6360_show("ldo2_en", MT6360_LDO_SLAVE_ADDR, 0x11);
	mt6360_show("ldo2_vout", MT6360_LDO_SLAVE_ADDR, 0x15);
	mt6360_show("ldo3_en", MT6360_LDO_SLAVE_ADDR, 0x05);
	mt6360_show("ldo5_en", MT6360_LDO_SLAVE_ADDR, 0x0b);
}

/*
 * Turn the touch rail on: LDO2 to 3.3 V, then the enable bit. Then read the enable register
 * back, so the log says whether the PMU took the write rather than only that it did not complain.
 */
static void mt6360_enable_touch_rail(void)
{
	out("mt6360: I2C5, LDO2 (vtp)...\n");

	mtk_i2c5_ungate_clocks();
	i2c5_pin_config();
	i2c5_pin_mode();
	mtk_i2c5_init_hw();

	mt6360_dump_state();

	uint8_t vout = 0, en = 0, en_before = 0;

	/* what the register holds before we touch it: the enable write below is a read-modify-write,
	 * so the bits that are not the enable bit have to survive */
	int r_before = mt6360_read_reg(MT6360_LDO_SLAVE_ADDR, MT6360_LDO2_EN_REG, &en_before);
	out("mt6360: ldo2 enable before=");
	if (r_before == 0) {
		out_hex(en_before);
	} else {
		out("ERR");
		out_dec((uint32_t)-r_before);
	}
	out("\n");

	int r_vout = mt6360_write_reg(MT6360_LDO_SLAVE_ADDR, MT6360_LDO2_VOUT_REG, MT6360_LDO2_VOUT_3V3);
	out("mt6360: vout write ");
	if (r_vout == 0) {
		out("ok");
	} else {
		out("FAIL(");
		out_dec((uint32_t)-r_vout);
		out(")");
	}
	out("\n");

	/* the enable write is a read-modify-write: the register holds more than the enable bit
	 * (the mode bits and the always-on flag), and the vendor driver only sets its own bit. */
	uint8_t en_new = (uint8_t)(en_before | MT6360_LDO2_EN_MASK);
	int r_en = mt6360_write_reg(MT6360_LDO_SLAVE_ADDR, MT6360_LDO2_EN_REG, en_new);
	out("mt6360: enable write ");
	if (r_en == 0) {
		out("ok");
	} else {
		out("FAIL(");
		out_dec((uint32_t)-r_en);
		out(")");
	}
	out("\n");

	int r_rd = mt6360_read_reg(MT6360_LDO_SLAVE_ADDR, MT6360_LDO2_EN_REG, &en);
	out("mt6360: enable read ");
	if (r_rd == 0) {
		out("ok, reg=");
		out_hex(en);
	} else {
		out("FAIL(");
		out_dec((uint32_t)-r_rd);
		out(")");
	}
	out("\n");

	r_rd = mt6360_read_reg(MT6360_LDO_SLAVE_ADDR, MT6360_LDO2_VOUT_REG, &vout);
	out("mt6360: vout read ");
	if (r_rd == 0) {
		out("ok, reg=");
		out_hex(vout);
	} else {
		out("FAIL(");
		out_dec((uint32_t)-r_rd);
		out(")");
	}
	out("\n");
}

#endif /* MT6360_H */
