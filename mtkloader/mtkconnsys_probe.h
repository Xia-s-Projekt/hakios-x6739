/*
 * Wi-Fi step 1 for the X6739 (MT6893): what the phone's own tree says about the CONNSYS,
 * and what the power manager says about the conn infra's domain -- read only.
 *
 * The shape is the reference port's (hakios-mt6768, kernel/bsd/dev/redwood_connsys.c, and
 * its loader's mtkloader/mtkconnsys.h): the tree is printed, the state of the hardware is
 * read, and nothing is written. The numbers are this board's (mtkconnsys6893.h).
 *
 * What is read here:
 *
 *   - /soc/consys@18000000 (compatible "mediatek,mt6893-consys") and its sixteen "reg"
 *     pairs, in the order MediaTek's enum consys_base_addr_index reads them, with the
 *     block's name against each address;
 *   - /soc/wifi@18000000 (compatible "mediatek,wifi"): the radio's AXI window and its two
 *     interrupts;
 *   - SPM (0x10006000), which is always on: the power status of the conn_infra domain,
 *     the power control register and the domain's software reset;
 *   - and only if the power status says the domain is on, the conn infra's own chip id
 *     (CONN_INFRA_CFG + 0, expected 0x20010101).
 *
 * The conn infra's registers hang the bus when the domain is off -- the reference port
 * learned that on the MT6768, where a read of a powered-off block ended the kernel -- so
 * the chip id is read only when SPM says the domain is up.
 */

#include <stdint.h>

#include "mtkconnsys6893.h"

#define FDT68_MAGIC       0xd00dfeedu
#define FDT68_BEGIN_NODE  1
#define FDT68_END_NODE    2
#define FDT68_PROP        3
#define FDT68_NOP         4
#define FDT68_END         9

/* the block names, in the order the vendor enum reads them */
static const char *const mt6893_connsys_blocks[] = {
	"CONN_INFRA_RGU", "CONN_INFRA_CFG", "CONN_HOST_CSR_TOP", "INFRACFG_AO",
	"TOPRGU", "SPM", "INFRACFG", "CONN_WT_SLP_CTL", "CONN_AFE_CTL",
	"CONN_INFRA_SYSRAM", "GPIO", "CONN_RF_SPI_MST", "CONN_SEMAPHORE",
	"CONN_TOP_THERM_CTL", "IOCFG_RT", "CONN_DEBUG_CTRL",
};

static uint32_t
mt6893_be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static int
mt6893_strlen(const char *s)
{
	int n = 0;

	while (s[n]) {
		n++;
	}
	return n;
}

static int
mt6893_streq(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

/* the string at `needle` anywhere in the first `len` bytes of `hay` */
static int
mt6893_contains(const uint8_t *hay, uint32_t len, const char *needle)
{
	for (uint32_t i = 0; i < len; i++) {
		uint32_t j = 0;

		while (needle[j] && i + j < len && hay[i + j] == needle[j]) {
			j++;
		}
		if (!needle[j]) {
			return 1;
		}
	}
	return 0;
}

/*
 * Walk the tree once and report the two nodes this step is about: the conn infra's own
 * node and the radio's. The walk is the same shape as the reference loader's: the device
 * tree's structure block, the names from the strings block, and the properties as they
 * come.
 */
static void
mt6893_connsys_dt_report(uint64_t dtb)
{
	const uint8_t *fdt = (const uint8_t *)(uintptr_t)dtb;
	const uint8_t *structs, *p, *end;
	const char *strings;
	/* the node being walked, and its parent's cell sizes */
	struct {
		const char *name;
		const uint8_t *compatible, *reg, *interrupts;
		uint32_t compatible_len, reg_len, interrupts_len;
		uint32_t address_cells, size_cells;
	} level[24];
	int depth = -1;

	if (dtb == 0 || mt6893_be32(fdt) != FDT68_MAGIC) {
		out("connsys: no flattened device tree from LK\n");
		return;
	}
	structs = fdt + mt6893_be32(fdt + 8);
	strings = (const char *)(fdt + mt6893_be32(fdt + 12));
	end = structs + mt6893_be32(fdt + 36);
	out("connsys: the phone's own tree, the MT6893's CONNSYS:\n");
	for (p = structs; p < end;) {
		uint32_t token = mt6893_be32(p);

		p += 4;
		if (token == FDT68_BEGIN_NODE) {
			const char *name = (const char *)p;

			p += (mt6893_strlen(name) + 1 + 3) & ~3u;
			if (++depth >= 24) {
				out("connsys: the tree is deeper than this walk keeps; stopped\n");
				return;
			}
			level[depth].name = name;
			level[depth].compatible = level[depth].reg = level[depth].interrupts = NULL;
			level[depth].compatible_len = level[depth].reg_len = level[depth].interrupts_len = 0;
			level[depth].address_cells = 2;
			level[depth].size_cells = 2;
		} else if (token == FDT68_PROP) {
			uint32_t len = mt6893_be32(p), name_offset = mt6893_be32(p + 4);
			const char *prop = strings + name_offset;
			const uint8_t *value = p + 8;

			p += 8 + ((len + 3) & ~3u);
			if (depth < 0) {
				continue;
			}
			if (mt6893_streq(prop, "compatible")) {
				level[depth].compatible = value;
				level[depth].compatible_len = len;
			} else if (mt6893_streq(prop, "reg")) {
				level[depth].reg = value;
				level[depth].reg_len = len;
			} else if (mt6893_streq(prop, "interrupts")) {
				level[depth].interrupts = value;
				level[depth].interrupts_len = len;
			}
		} else if (token == FDT68_END_NODE) {
			if (depth < 0) {
				break;
			}
			int is_connsys = level[depth].compatible &&
			    mt6893_contains(level[depth].compatible, level[depth].compatible_len,
			        "mt6893-consys");
			int is_wifi = level[depth].compatible &&
			    mt6893_contains(level[depth].compatible, level[depth].compatible_len,
			        "mediatek,wifi");

			if (is_connsys && level[depth].reg) {
				out("  consys (the conn infra): ");
				/* the "reg" property: two cells of address and two of size each, 64 bits */
				for (uint32_t at = 0; at + 16 <= level[depth].reg_len; at += 16) {
					uint64_t base = ((uint64_t)mt6893_be32(level[depth].reg + at) << 32) |
					    mt6893_be32(level[depth].reg + at + 4);
					uint64_t size = ((uint64_t)mt6893_be32(level[depth].reg + at + 8) << 32) |
					    mt6893_be32(level[depth].reg + at + 12);
					unsigned index = at / 16;

					out("\n    ");
					out(index < sizeof(mt6893_connsys_blocks) / sizeof(mt6893_connsys_blocks[0]) ?
					    mt6893_connsys_blocks[index] : "(a block the vendor enum does not name)");
					out(" at ");
					out_hex(base);
					out(", ");
					out_hex(size);
					out(" bytes");
				}
				out("\n");
			}
			if (is_wifi) {
				out("  wifi (the radio): ");
				if (level[depth].reg && level[depth].reg_len >= 16) {
					uint64_t base = ((uint64_t)mt6893_be32(level[depth].reg) << 32) |
					    mt6893_be32(level[depth].reg + 4);
					uint64_t size = ((uint64_t)mt6893_be32(level[depth].reg + 8) << 32) |
					    mt6893_be32(level[depth].reg + 12);

					out("its AXI window at ");
					out_hex(base);
					out(" + ");
					out_hex(size);
					out(" bytes");
				}
				if (level[depth].interrupts) {
					out("; its interrupts:");
					/* GIC-v3: four cells each, of which the second is the SPI number */
					for (uint32_t at = 0; at + 16 <= level[depth].interrupts_len; at += 16) {
						out(" SPI ");
						out_dec(mt6893_be32(level[depth].interrupts + at + 4));
					}
				}
				out("\n");
			}
			depth--;
		} else if (token == FDT68_NOP) {
			continue;
		} else {
			break;                              /* FDT_END, or something this walk does not know */
		}
	}
}

/*
 * Step 1's other half: the state of the conn infra's domain, read from SPM, which is
 * always on. The conn infra's own registers are read only when the power status says the
 * domain is up.
 */
static void
mt6893_connsys_state(void)
{
	uint32_t status = rd32(CONNSYS_SPM_BASE + SPM_PWR_STATUS);
	uint32_t status2 = rd32(CONNSYS_SPM_BASE + SPM_PWR_STATUS_2ND);
	uint32_t pwr = rd32(CONNSYS_SPM_BASE + SPM_CONN_PWR_CON);
	uint32_t reset = rd32(CONNSYS_TOPRGU_BASE + TOPRGU_WDT_SWSYSRST);
	int on = (status & (1u << 1)) && (status2 & (1u << 1));

	out("connsys: SPM: PWR_STATUS ");
	out_hex(status);
	out(", PWR_STATUS_2ND ");
	out_hex(status2);
	out(", CONN_PWR_CON ");
	out_hex(pwr);
	out("; TOP_RGU WDT_SWSYSRST ");
	out_hex(reset);
	out(": the conn_infra domain is ");
	out(on ? "ON" : "off");
	out("\n");
	if (!on) {
		out("connsys: its own registers are not read: a powered-off block of this kind hangs the bus\n");
		return;
	}
	uint32_t version = rd32(CONNSYS_INFRA_CFG_BASE + CONNSYS_HW_VER);

	out("connsys: the chip id is ");
	out_hex(version);
	out(version == CONNSYS_HW_VER_VALUE ? " (the MT6893's)\n" : " (NOT 0x20010101)\n");
	out("connsys: the configuration id is ");
	out_hex(rd32(CONNSYS_INFRA_CFG_BASE + CONNSYS_CFG_ID));
	out("\n");
}
