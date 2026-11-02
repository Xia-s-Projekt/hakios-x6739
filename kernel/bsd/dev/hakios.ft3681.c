/*
 * The touch screen of the X6739 (Infinix GT 10 Pro): a FocalTech FT3681 on the MT6893's I2C0,
 * as a source of touches for /dev/hakios-touch (sys/hakios.input.h). Only with boot-arg
 * touchscreen=1.
 *
 * The loader (mtkloader/mtki2c.h) brings the controller and the chip up before the kernel
 * starts: the touch rail (MT6360 LDO2) is switched on, the chip is reset and released, and the
 * chip's answers are read. What is left for this driver is to read what the chip says, in the
 * same way the phone's own driver does.
 *
 * There is no clock, pin, regulator or I2C framework in this kernel, so the driver does all of
 * it itself, as the Goodix driver beside it does for its bus:
 *   bus      the MT6893's I2C0 controller at 0x11015000, its registers 16 bits wide, driven by
 *            the APDMA engine at 0x11000080; the timings are the ones the phone's own kernel
 *            uses for 400 kHz off a 124.8 MHz parent (TIMING 0x0419, LTIMING 0x0123, EXT_CONF
 *            0x5801, SDA_TIMING 0x100a)
 *   pins     SCL0 is GPIO204 and SDA0 GPIO205, both in the IOCFG_LT block at 0x11f20000, with
 *            the 203 ohm pull-ups the phone's own pinconf shows
 *   chip     the report is at register 0x01: write that one byte, read 62 bytes back
 * No interrupt is used: a thread reads the report every 5 ms (boot-arg touch_poll_us=N), the
 * same way the Goodix driver does, and every report it reads is counted in the log.
 *
 * The report: two header bytes and ten six-byte points, the point block starting at byte 2.
 * Byte 1's high nibble is the report type, its low nibble the number of points. A point:
 *   b0: bits 7.6 = event (2 = contact), bits 3.0 = x[15:12]
 *   b1: x[11:4]
 *   b2: bits 7.4 = finger id, bits 3.0 = y[15:12]
 *   b3: y[11:4]
 *   b4: bits 7.4 = x[3:0], bits 3.0 = y[3:0]
 *   b5: area
 * so x = ((b0 & 0x0f) << 12) | (b1 << 4) | (b4 >> 4) and y = ((b2 & 0x0f) << 12) | (b3 << 4) |
 * (b4 & 0x0f). The coordinates are the panel's 1080 x 2400 in sixteenths of a pixel (the
 * device tree's focaltech,display-coords is <0 0 17279 38399>). A report whose every byte is
 * 0xff carries no data: the panel has nothing to say.
 *
 * Register layouts: Linux 6.1 drivers/i2c/busses/i2c-mt65xx.c (the mt6873_compat profile, which
 * is the one the phone's own kernel uses: the v2 register table, the slave address at 0x04),
 * Linux drivers/clk/mediatek/clk-mt6885-i2c.c and clk-mt6885.c, Linux
 * drivers/pinctrl/mediatek/pinctrl-mt6885.c, and the phone's own device tree.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern void FlushPoC_DcacheRegion(vm_offset_t va, size_t length);
extern uint64_t kvtophys(vm_offset_t va);       /* pmap_paddr_t */

/* ---- the bus and the pins ---- */

#define I2C0_PHYS              0x11015000UL
#define I2C0_SIZE              0x1000UL
#define I2C0_DMA_PHYS          0x11000080UL
#define I2C0_DMA_SIZE          0x1000UL
#define GPIO_PHYS              0x10005000UL
#define GPIO_SIZE              0x1000UL
#define IOCFG_LT_PHYS          0x11f20000UL
#define IOCFG_LT_SIZE          0x1000UL
#define TOPCKGEN_PHYS          0x10000000UL
#define TOPCKGEN_SIZE          0x1000UL
#define INFRACFG_PHYS          0x10001000UL
#define INFRACFG_SIZE          0x1000UL
#define IMP_IIC_WRAP_C_PHYS    0x11008000UL
#define IMP_IIC_WRAP_C_SIZE    0x1000UL

#define FTS_SLAVE_ADDR         0x38        /* the device tree's reg */
#define FTS_REG_REPORT         0x01        /* the touch report is at this register */
#define FTS_REPORT_LEN         62
#define FTS_REPORT_HEADER      2
#define FTS_POINT_LEN          6
#define FTS_MAX_POINTS         10

/* the panel: 1080 x 2400, in sixteenths of a pixel (the device tree's focaltech,display-coords) */
#define PANEL_X_MAX            17279
#define PANEL_Y_MAX            38399
#define PANEL_W                1080
#define PANEL_H                2400

/* the controller's registers, 16 bits wide */
#define I2C_REG_PORT           0x00UL
#define I2C_REG_SLAVE_ADDR     0x04UL
#define I2C_REG_INTR_MASK      0x08UL
#define I2C_REG_INTR_STAT      0x0cUL
#define I2C_REG_CONTROL        0x10UL
#define I2C_REG_TRANSFER_LEN   0x14UL
#define I2C_REG_TRANSAC_LEN    0x18UL
#define I2C_REG_DELAY_LEN      0x1cUL
#define I2C_REG_TIMING         0x20UL
#define I2C_REG_START          0x24UL
#define I2C_REG_EXT_CONF       0x28UL
#define I2C_REG_LTIMING        0x2cUL
#define I2C_REG_HS             0x30UL
#define I2C_REG_IO_CONFIG      0x34UL
#define I2C_REG_FIFO_ADDR_CLR  0x38UL
#define I2C_REG_SDA_TIMING     0x3cUL
#define I2C_REG_TRANSFER_AUX   0x44UL
#define I2C_REG_CLOCK_DIV      0x48UL
#define I2C_REG_SOFTRESET      0x50UL
#define I2C_REG_SCL_MIS_COMP   0x90UL
#define I2C_REG_DEBUGSTAT      0xe4UL
#define I2C_REG_DEBUGCTRL      0xe8UL
#define I2C_REG_FIFO_STAT      0xf4UL

/* the APDMA engine's registers, 32 bits wide */
#define APDMA_REG_INT_FLAG     0x00UL
#define APDMA_REG_EN           0x08UL
#define APDMA_REG_RST          0x0cUL
#define APDMA_REG_CON          0x18UL
#define APDMA_REG_TX_MEM_ADDR  0x1cUL
#define APDMA_REG_RX_MEM_ADDR  0x20UL
#define APDMA_REG_TX_LEN       0x24UL
#define APDMA_REG_RX_LEN       0x28UL
#define APDMA_REG_TX_4G_MODE   0x54UL
#define APDMA_REG_RX_4G_MODE   0x58UL

#define I2C_CTL_RS             (1u << 1)
#define I2C_CTL_DMA_EN         (1u << 2)
#define I2C_CTL_CLK_EXT_EN     (1u << 3)
#define I2C_CTL_DIR_CHANGE     (1u << 4)
#define I2C_CTL_ACKERR_DET_EN  (1u << 5)
#define I2C_CTL_DMAACK_EN      (1u << 8)
#define I2C_CTL_ASYNC_MODE     (1u << 9)

#define I2C_STAT_TRANSAC_COMP  (1u << 0)
#define I2C_STAT_ACKERR        (1u << 1)
#define I2C_STAT_HS_NACKERR    (1u << 2)
#define I2C_STAT_ARB_LOST      (1u << 3)
#define I2C_STAT_RS_TRANSFER   (1u << 4)
#define I2C_STAT_CONFERR       (1u << 9)
#define I2C_STAT_ALL           (I2C_STAT_TRANSAC_COMP | I2C_STAT_ACKERR | I2C_STAT_HS_NACKERR | \
                                I2C_STAT_ARB_LOST | I2C_STAT_RS_TRANSFER | I2C_STAT_CONFERR)

#define I2C_TRANSAC_START      (1u << 0)

#define I2C_DMA_ASYNC_MODE     0x0004u
#define I2C_DMA_SKIP_CONFIG    0x0010u
#define I2C_DMA_DIR_CHANGE     0x0200u
#define I2C_DMA_START_EN       0x0001u
#define I2C_DMA_WARM_RST       0x0001u
#define I2C_DMA_HARD_RST       0x0002u
#define I2C_DMA_HANDSHAKE_RST  0x0004u
#define I2C_HANDSHAKE_RST      0x0020u
#define I2C_SOFT_RST           0x0001u

/* the timings for 400 kHz off a 124.8 MHz parent (Linux's mtk_i2c_set_speed for mt6873_compat) */
#define I2C0_TIMING            0x0419u
#define I2C0_LTIMING           0x0123u
#define I2C0_EXT_CONF          0x5801u
#define I2C0_SDA_TIMING        0x100au

/* the pins: SCL0 is GPIO204, SDA0 GPIO205 (Linux's pinctrl-mt6885.c) */
#define I2C0_PIN_MODE_REG      0x490UL
#define I2C0_PIN_BITS          ((1u << 12) | (1u << 13))
#define MWR_OFFSET             0x0cUL        /* the mode register's modification-enable bit */

static vm_offset_t i2c0, i2c0_dma, gpio, iocfg_lt, topckgen, infracfg, imp_iic;

static inline uint16_t
rd16(vm_offset_t base, uint32_t at)
{
	return *(volatile uint16_t *)(base + at);
}

static inline void
wr16(vm_offset_t base, uint32_t at, uint16_t value)
{
	*(volatile uint16_t *)(base + at) = value;
}

static inline uint32_t
rd32(vm_offset_t base, uint32_t at)
{
	return *(volatile uint32_t *)(base + at);
}

static inline void
wr32(vm_offset_t base, uint32_t at, uint32_t value)
{
	*(volatile uint32_t *)(base + at) = value;
}

static void
sleep_us(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

/* ---- the pins and the clocks ---- */

/*
 * The pull-ups on the two pins live in the IOCFG_LT block, one register per group, with a bit
 * per pin: 204 is the lower bit, 205 the next. The settings are the phone's own: a 203 ohm
 * pull-up (rsel 3, R0 and R1 both set), the input buffers on, the pull-downs off, the schmitt
 * triggers on, the drive strength at 7.
 */
static void
i2c0_pin_pull_up(void)
{
	wr32(iocfg_lt, 0x0044, (1u << 15) | (1u << 16));       /* IES: the input buffers */
	wr32(iocfg_lt, 0x0068, (1u << 15) | (1u << 16));       /* PD: the pull-downs off */
	wr32(iocfg_lt, 0x0084, (1u << 15) | (1u << 16));       /* PU: the pull-ups on */
	wr32(iocfg_lt, 0x00e4, (1u << 7) | (1u << 8));         /* SMT: the schmitt triggers */
	wr32(iocfg_lt, 0x00d4, 0x0f);                          /* R0/R1: rsel 3 */
	wr32(iocfg_lt, 0x0024, (7u << 0) | (7u << 3));         /* EH: the drive strength */
}

/* The two pins to their I2C function: the mode register's MSB is the modification-enable bit. */
static void
i2c0_pin_mode(uint32_t func)
{
	uint32_t value = ((0x8u | (func & 7u)) << 16) | ((0x8u | (func & 7u)) << 20);

	wr32(gpio, I2C0_PIN_MODE_REG + MWR_OFFSET, value);
}

/* The controller's clock: i2c_sel to univpll_d5_d4 (124.8 MHz), the gate open. */
static void
i2c0_ungate_clocks(void)
{
	wr32(topckgen, 0x0b8, 3u << 8);
	wr32(topckgen, 0x0b4, 2u << 8);
	wr32(topckgen, 0x0b8, 1u << 15);
	wr32(topckgen, 0x008, 1u << 10);

	wr32(imp_iic, 0xe04, 1u << 0);
	wr32(infracfg, 0x084, 1u << 11);
	wr32(infracfg, 0x08c, 1u << 18);
	wr32(infracfg, 0x0d4, 1u << 31);
	__asm__ volatile ("dsb sy" ::: "memory");
	sleep_us(10);
}

/* The controller's registers, in Linux's order (mtk_i2c_init_hw, then the timing adjustment). */
static void
i2c0_init_hw(void)
{
	wr16(i2c0, I2C_REG_START, 0);
	wr16(i2c0, I2C_REG_INTR_STAT, rd16(i2c0, I2C_REG_INTR_STAT));

	wr32(i2c0_dma, APDMA_REG_RST, I2C_DMA_WARM_RST);
	sleep_us(2);
	wr32(i2c0_dma, APDMA_REG_RST, 0);
	sleep_us(2);
	wr32(i2c0_dma, APDMA_REG_RST, I2C_DMA_HANDSHAKE_RST | I2C_DMA_HARD_RST);
	wr16(i2c0, I2C_REG_SOFTRESET, I2C_HANDSHAKE_RST | I2C_SOFT_RST);
	sleep_us(2);
	wr32(i2c0_dma, APDMA_REG_RST, 0);
	wr16(i2c0, I2C_REG_SOFTRESET, 0);

	wr16(i2c0, I2C_REG_IO_CONFIG, 0x0003);         /* open drain */
	wr16(i2c0, I2C_REG_TIMING, I2C0_TIMING);
	wr16(i2c0, I2C_REG_HS, 0);
	wr16(i2c0, I2C_REG_LTIMING, I2C0_LTIMING);

	wr16(i2c0, I2C_REG_EXT_CONF, I2C0_EXT_CONF);
	wr16(i2c0, I2C_REG_CLOCK_DIV, 0);
	wr16(i2c0, I2C_REG_SCL_MIS_COMP, 0);
	wr16(i2c0, I2C_REG_SDA_TIMING, I2C0_SDA_TIMING);
	wr16(i2c0, I2C_REG_TIMING, I2C0_TIMING);
	wr16(i2c0, I2C_REG_HS, 0);
	wr16(i2c0, I2C_REG_LTIMING, I2C0_LTIMING);

	wr16(i2c0, I2C_REG_CONTROL, I2C_CTL_ACKERR_DET_EN | I2C_CTL_DMA_EN |
	      I2C_CTL_DMAACK_EN | I2C_CTL_ASYNC_MODE);
	wr16(i2c0, I2C_REG_DELAY_LEN, 2);
}

/* ---- a transfer ---- */

/* The DMA buffers: one page, physically contiguous, so the engine can reach them. */
static uint8_t *dma_buffer;
static vm_offset_t dma_buffer_pa;

static bool
dma_buffer_ready(void)
{
	vm_offset_t data = 0;

	if (dma_buffer != NULL) {
		return true;
	}
	if (kmem_alloc_contig(kernel_map, &data, PAGE_SIZE, PAGE_MASK, 0, 0,
	                      KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return false;
	}
	dma_buffer = (uint8_t *)data;
	dma_buffer_pa = kvtophys(data);
	return dma_buffer != NULL;
}

static int
i2c0_wait(void)
{
	uint16_t seen = 0;
	uint32_t rs_polls = 0;

	for (uint32_t t = 0; t < 50000; t++) {
		uint16_t stat = rd16(i2c0, I2C_REG_INTR_STAT);

		if (stat) {
			wr16(i2c0, I2C_REG_INTR_STAT, stat);
			seen |= stat;
		}
		if (seen & (I2C_STAT_ACKERR | I2C_STAT_HS_NACKERR | I2C_STAT_ARB_LOST)) {
			return -2;
		}
		if (seen & I2C_STAT_TRANSAC_COMP) {
			return 0;
		}
		/* Linux's handler completes on RS_TRANSFER as well, under auto_restart */
		if ((seen & I2C_STAT_RS_TRANSFER) && ++rs_polls > 2000) {
			return 0;
		}
		sleep_us(1);
	}
	return -3;
}

static void
i2c0_prepare(uint8_t slave_addr, uint32_t tx_len, uint32_t rx_len)
{
	uint16_t ctl = rd16(i2c0, I2C_REG_CONTROL) &
		~(I2C_CTL_DIR_CHANGE | I2C_CTL_RS | I2C_CTL_DMA_EN | I2C_CTL_DMAACK_EN |
		  I2C_CTL_ASYNC_MODE | I2C_CTL_CLK_EXT_EN);

	ctl |= I2C_CTL_DIR_CHANGE | I2C_CTL_RS | I2C_CTL_DMA_EN | I2C_CTL_DMAACK_EN | I2C_CTL_ASYNC_MODE;
	wr16(i2c0, I2C_REG_CONTROL, ctl);
	wr16(i2c0, I2C_REG_SLAVE_ADDR, (uint16_t)(slave_addr << 1));
	wr16(i2c0, I2C_REG_INTR_STAT, I2C_STAT_ALL);
	wr16(i2c0, I2C_REG_FIFO_ADDR_CLR, 1);
	wr16(i2c0, I2C_REG_INTR_MASK, 0);
	wr16(i2c0, I2C_REG_TRANSFER_LEN, (uint16_t)tx_len);
	wr16(i2c0, I2C_REG_TRANSFER_AUX, (uint16_t)rx_len);
	wr16(i2c0, I2C_REG_TRANSAC_LEN, 2);
}

/*
 * One transfer over the DMA engine: tx_len bytes written, rx_len bytes read. The engine needs
 * the buffers in memory it can reach, so they are cleaned before and invalidated after.
 */
static int
i2c0_transfer(const uint8_t *tx, uint32_t tx_len, uint8_t *rx, uint32_t rx_len)
{
	uint8_t *tx_buffer, *rx_buffer;
	int result;

	if (!dma_buffer_ready()) {
		return -1;
	}
	tx_buffer = dma_buffer;
	rx_buffer = dma_buffer + 256;

	for (uint32_t i = 0; i < tx_len; i++) {
		tx_buffer[i] = tx[i];
	}
	for (uint32_t i = 0; i < rx_len; i++) {
		rx_buffer[i] = 0;
	}
	FlushPoC_DcacheRegion((vm_offset_t)tx_buffer, 256);
	FlushPoC_DcacheRegion((vm_offset_t)rx_buffer, 256);

	/* apdma_sync and a read after a write: warm-reset the engine, as the 6.1 driver does */
	wr32(i2c0_dma, APDMA_REG_RST, I2C_DMA_HANDSHAKE_RST | I2C_DMA_WARM_RST);
	for (uint32_t t = 0; t < 100 && (rd32(i2c0_dma, APDMA_REG_RST) & I2C_DMA_WARM_RST); t++) {
		sleep_us(1);
	}
	wr32(i2c0_dma, APDMA_REG_RST, 0);
	wr16(i2c0, I2C_REG_SOFTRESET, I2C_HANDSHAKE_RST);
	wr16(i2c0, I2C_REG_SOFTRESET, 0);

	i2c0_prepare(FTS_SLAVE_ADDR, tx_len, rx_len);

	wr32(i2c0_dma, APDMA_REG_INT_FLAG, 0);
	wr32(i2c0_dma, APDMA_REG_CON, I2C_DMA_SKIP_CONFIG | I2C_DMA_ASYNC_MODE | I2C_DMA_DIR_CHANGE);
	wr32(i2c0_dma, APDMA_REG_TX_4G_MODE, (uint32_t)(dma_buffer_pa >> 32));
	wr32(i2c0_dma, APDMA_REG_TX_MEM_ADDR, (uint32_t)dma_buffer_pa);
	wr32(i2c0_dma, APDMA_REG_TX_LEN, tx_len);
	wr32(i2c0_dma, APDMA_REG_RX_4G_MODE, (uint32_t)((dma_buffer_pa + 256) >> 32));
	wr32(i2c0_dma, APDMA_REG_RX_MEM_ADDR, (uint32_t)(dma_buffer_pa + 256));
	wr32(i2c0_dma, APDMA_REG_RX_LEN, rx_len);
	__asm__ volatile ("dsb sy" ::: "memory");
	wr32(i2c0_dma, APDMA_REG_EN, I2C_DMA_START_EN);
	__asm__ volatile ("dsb sy" ::: "memory");
	wr16(i2c0, I2C_REG_START, I2C_TRANSAC_START);

	result = i2c0_wait();
	if (result) {
		return result;
	}
	FlushPoC_DcacheRegion((vm_offset_t)rx_buffer, 256);
	for (uint32_t i = 0; i < rx_len; i++) {
		rx[i] = rx_buffer[i];
	}
	return 0;
}

/* ---- the report ---- */

/* 1 when every byte of the report is 0xff: the panel has nothing to say. */
static bool
report_is_idle(const uint8_t *report)
{
	for (uint32_t i = 0; i < FTS_REPORT_LEN; i++) {
		if (report[i] != 0xff) {
			return false;
		}
	}
	return true;
}

struct fts_point {
	uint32_t event;         /* bits 7.6 of b0: 2 = contact */
	uint32_t id;            /* bits 7.4 of b2 */
	uint32_t x, y;
	uint32_t area;          /* b5 */
};

static void
decode_point(const uint8_t *p, struct fts_point *out)
{
	out->event = (p[0] >> 6) & 3u;
	out->id = (p[2] >> 4) & 0xfu;
	out->x = ((p[0] & 0x0fu) << 12) | (p[1] << 4) | (p[4] >> 4);
	out->y = ((p[2] & 0x0fu) << 12) | (p[3] << 4) | (p[4] & 0x0fu);
	out->area = p[5];
}

/*
 * The points of a report, in order. The header says how many there are; a slot is unused when
 * its b2 is above 0x9f, and that is where the list ends.
 */
static uint32_t
decode_report(const uint8_t *report, struct fts_point *out, uint32_t max)
{
	uint32_t count = report[1] & 0x0fu;
	uint32_t n = 0;

	for (uint32_t i = 0; i < FTS_MAX_POINTS && i < max; i++) {
		const uint8_t *p = &report[FTS_REPORT_HEADER + i * FTS_POINT_LEN];

		if (i >= count) {
			break;
		}
		if (p[2] > 0x9f) {
			break;
		}
		decode_point(p, &out[n]);
		n++;
	}
	return n;
}

/* The panel's own geometry: the report is in sixteenths of a pixel, the queue wants 0..65535. */
static uint32_t
scale_x(uint32_t x)
{
	return x >= PANEL_X_MAX ? 65535 : x * 65535 / PANEL_X_MAX;
}

static uint32_t
scale_y(uint32_t y)
{
	return y >= PANEL_Y_MAX ? 65535 : y * 65535 / PANEL_Y_MAX;
}

/* ---- all of it together ---- */

static struct {
	bool down;
	uint32_t x, y;
} contact[FTS_MAX_POINTS];

static uint32_t polls, reports, points, bad_reports, idle_reports;

static void
report_to_touches(const uint8_t *report)
{
	struct fts_point pts[FTS_MAX_POINTS];
	uint32_t n;

	for (int i = 0; i < FTS_MAX_POINTS; i++) {
		contact[i].down = false;      /* until the report says otherwise */
	}
	n = decode_report(report, pts, FTS_MAX_POINTS);
	for (uint32_t i = 0; i < n; i++) {
		const struct fts_point *p = &pts[i];
		uint32_t id = p->id;
		uint32_t x, y;

		if (id >= FTS_MAX_POINTS) {
			continue;
		}
		if (p->event != 2) {         /* 2 = a contact; the others are a release */
			continue;
		}
		x = scale_x(p->x);
		y = scale_y(p->y);
		points++;
		if (!contact[id].down) {
			hakios_touch_post(HAKIOS_TOUCH_DOWN, id, x, y);
			contact[id].down = true;
		} else if (contact[id].x != x || contact[id].y != y) {
			hakios_touch_post(HAKIOS_TOUCH_MOVE, id, x, y);
		}
		contact[id].x = x;
		contact[id].y = y;
	}
	/* a finger that is no longer in the report has lifted */
	for (uint32_t id = 0; id < FTS_MAX_POINTS; id++) {
		if (contact[id].down) {
			contact[id].down = false;
			hakios_touch_post(HAKIOS_TOUCH_UP, id, contact[id].x, contact[id].y);
		}
	}
}

static void
ft3681_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint8_t report[FTS_REPORT_LEN];
	uint8_t command = FTS_REG_REPORT;
	uint32_t poll_us = 5000, flips = 0;

	PE_parse_boot_argn("touch_poll_us", &poll_us, sizeof(poll_us));
	if (poll_us < 250 || poll_us > 100000) {
		poll_us = 5000;
	}
	PE_parse_boot_argn("touch_flip", &flips, sizeof(flips));

	i2c0 = ml_io_map(I2C0_PHYS, I2C0_SIZE);
	i2c0_dma = ml_io_map(I2C0_DMA_PHYS, I2C0_DMA_SIZE);
	gpio = ml_io_map(GPIO_PHYS, GPIO_SIZE);
	iocfg_lt = ml_io_map(IOCFG_LT_PHYS, IOCFG_LT_SIZE);
	topckgen = ml_io_map(TOPCKGEN_PHYS, TOPCKGEN_SIZE);
	infracfg = ml_io_map(INFRACFG_PHYS, INFRACFG_SIZE);
	imp_iic = ml_io_map(IMP_IIC_WRAP_C_PHYS, IMP_IIC_WRAP_C_SIZE);

	i2c0_ungate_clocks();
	i2c0_pin_pull_up();
	i2c0_pin_mode(1);
	i2c0_init_hw();

	/* the chip's identity, so that the log says what is on the other end of the bus */
	{
		uint8_t id_high = 0, id_low = 0, fw = 0;

		if (i2c0_transfer(&command, 1, &id_high, 1) == 0) {
			command = 0xa3;
			i2c0_transfer(&command, 1, &id_high, 1);
			command = 0x9f;
			i2c0_transfer(&command, 1, &id_low, 1);
			command = 0xa6;
			i2c0_transfer(&command, 1, &fw, 1);
			printf("ft3681: chip id %02x%02x, firmware %02x\n", id_high, id_low, fw);
		} else {
			printf("ft3681: the chip did not answer on I2C0\n");
		}
	}

	printf("ft3681: ready; a report is read every %u us\n", poll_us);

	for (unsigned tick = 1;; tick++) {
		polls++;
		if (i2c0_transfer(&command, 1, report, FTS_REPORT_LEN) == 0) {
			if (report_is_idle(report)) {
				idle_reports++;
			} else {
				reports++;
				report_to_touches(report);
			}
		} else {
			bad_reports++;
			if (bad_reports < 8) {
				printf("ft3681: the report could not be read (stat %#x, debug %#x, fifo %#x)\n",
				    rd16(i2c0, I2C_REG_DEBUGSTAT), rd16(i2c0, I2C_REG_DEBUGSTAT),
				    rd16(i2c0, I2C_REG_FIFO_STAT));
				i2c0_init_hw();
			}
		}
		if (tick % 1200 == 0) {
			printf("ft3681: %u reads, %u reports (%u idle), %u points, %u failed\n",
			    polls, reports, idle_reports, points, bad_reports);
		}
		sleep_us(poll_us);
	}
}

void
hakios_ft3681_init(void)
{
	uint32_t enabled = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("touchscreen", &enabled, sizeof(enabled)) || !enabled) {
		return;
	}
	if (kernel_thread_start(ft3681_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
