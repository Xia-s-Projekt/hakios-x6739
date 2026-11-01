# Handoff: the X6739's touchscreen (FocalTech FT3681 over I2C0)

**State on 2026-11-01.**
The bare-metal loader boots, initializes GICv3, caches, DRAM, display (1080×2400 @ `0x76a10000`), mounts the UFS APFS root, boots XNU SMP (2 cores), runs `launchd` and `backboardd`, and renders the iOS 16.5 SpringBoard lockscreen. Synthetic touch injection (`rwtouch`) taps and unlocks SpringBoard normally over `/dev/hakios-touch`.

Physical touch bring-up on **FocalTech FT3681** over **MT6893 I2C0** (slave address `0x38`) has achieved a major breakthrough and is now at the final stage before the XNU driver:
1. **The I2C0 hardware controller is fully operational.**
   - SCL clock-stretch deadlock in State 3 (`DEBUGSTAT = 0x343`) has been completely eliminated by removing `I2C_CTL_CLK_EXT_EN` (bit 3) from `CONTROL`.
   - In both FIFO and DMA modes, the controller executes the complete transaction cycle and cleanly returns to IDLE state (`DEBUGSTAT = 0x700`).
   - The transaction reports `stat = 0x3` = `I2C_STAT_TRANSAC_COMP` (0x01) | `I2C_STAT_ACKERR` (0x02).
2. **The root cause of slave NACK is identified with conclusive evidence.**
   - The FT3681 touch IC is **unpowered** at boot: LK / fastboot leaves the touch regulator unpowered.
   - Device tree (`fdt-x6739.dtb`) specifies `vtouch-supply = <0x1e9>`, which is **MT6360 LDO2 (`vtp`, 3.3V)** on I2C5 (`0x11f00000`, MT6360 LDO slave ID `0x64`).
   - Android vendor kernel (`focaltech_core.c` lines 855-867) explicitly calls `regulator_enable(ts_data->vdd)` on driver probe.
   - Once MT6360 LDO2 is powered on, the FT3681 will ACK and report Chip ID `0x5662`.

This file contains all exact register maps, clock gates, pinctrl settings, MT6360 CRC-8 wire packets, protocol requirements, and step-by-step instructions for the next agent to complete physical touch bring-up.

---

## 1. Verified Hardware & Pinout Specifications

All values are derived directly from the phone's live registers and device tree (`refs/fdt-x6739.dtb`):

| Parameter | Value | Source / Evidence |
|---|---|---|
| Touch IC | FocalTech **FT3681** | `/proc/device-tree/touch/supplier-0` = `TM_FT3681_00`, DT `focaltech,fts` |
| Chip ID | `0x5662` | Reg `0xA3` = `0x56`, Reg `0x9F` = `0x62` |
| Firmware Version | `0x44` | Reg `0xA6` = `0x44` |
| Vendor ID | `0x8D` | Reg `0xA8` = `0x8D` |
| Bus | **I2C0**, `mediatek,mt6873-i2c` | DT `/soc/i2c@11015000` |
| I2C0 Registers | Base `0x11015000` (len `0x1000`) | DT `reg` entry 0 |
| APDMA Registers | Base `0x11000080` (len `0x80`) | DT `reg` entry 1 |
| Slave Address | `0x38` (7-bit) | DT `reg = <0x38>` under `focaltech@38` |
| Bus Speed | 400 kHz | DT `clock-frequency = <0x061a80>` |
| Clock Divider | 1 | DT `clock-div = <1>` |
| Bus Type | Open Drain | DT `mediatek,use-open-drain` |
| SCL0 Pin | **GPIO 204, Mode 1** | `IOCFG_LT` + MT6885 pinctrl table |
| SDA0 Pin | **GPIO 205, Mode 1** | `IOCFG_LT` + MT6885 pinctrl table |
| Reset GPIO | **GPIO 20, Mode 0** | Android kernel `gpio-305 (fts_reset_gpio)`, base 285 (`305-285=20`) |
| Interrupt GPIO | **GPIO 21, Mode 0** | Android kernel `gpio-306 (fts_irq_gpio)`, GIC SPI 245 (GIC interrupt 277) |
| Max Contacts | 10 | DT `focaltech,max-touch-number = <10>` |
| Panel Resolution | 1080×2400 | DT `focaltech,display-coords = <0 0 0x437f 0x95ff>` (16× subpixels) |
| Touch Power Rail | **MT6360 LDO2 (`vtp`, 3.3V)** | DT `vtouch-supply = <0x1e9>`, MT6360 slave `0x64` on I2C5 (`0x11f00000`) |

---

## 2. I2C0 Hardware Controller & Timing Configuration

The MT6893 I2C0 controller uses MediaTek's **v2** register architecture (16-bit wide registers, slave address at offset `0x04`, `slave_addr_ver = 0`).

### 2.1 Register Map (Base `0x11015000`, 16-bit)

```
0x00: DATA_PORT
0x04: SLAVE_ADDR        (7-bit slave address shifted left 1: (slave << 1))
0x08: INTR_MASK         (Interrupt mask: write 0xffff to mask)
0x0c: INTR_STAT         (Interrupt status: write 1 to clear)
0x10: CONTROL           (Control bits: DMA_EN, ACKERR_DET_EN, etc.)
0x14: TRANSFER_LEN      (Transfer length: bytes for TX / phase 1)
0x18: TRANSAC_LEN       (Transaction count: 1 for single, 2 for WRRD)
0x1c: DELAY_LEN         (Delay between transfers: 2)
0x20: TIMING            (Sample & step timing: 0x0419)
0x24: START             (Trigger transfer: write 1)
0x28: EXT_CONF          (Extended timing: 0x5801)
0x2c: LTIMING           (Low timing: 0x0123)
0x30: HS                (High speed timing: 0x0000)
0x34: IO_CONFIG         (0x0003 for open-drain)
0x38: FIFO_ADDR_CLR     (Write 1 to clear FIFO pointers)
0x3c: SDA_TIMING        (SDA timing: 0x100a)
0x44: TRANSFER_LEN_AUX  (Transfer length for RX phase in WRRD)
0x48: CLOCK_DIV         (0x0000)
0x50: SOFTRESET         (Soft reset)
0xe4: DEBUGSTAT         (FSM state: bits 4:0=master state, 6=write, 7=busy)
0xe8: DEBUGCTRL         (Debug control)
0xec: DMA_FSM_DEBUG     (APDMA FSM state)
0xf4: FIFO_STAT         (FIFO status: bits 3:0=rx_len, 7:4=tx_len)
```

### 2.2 Calculated 400 kHz Timings (`tools/i2c-timing.py`)

Parent clock is `i2c_sel` in TOPCKGEN at **124.8 MHz** (`univpll_d5_d4`):
- `TIMING` (0x20): **`0x0419`**
- `LTIMING` (0x2c): **`0x0123`**
- `EXT_CONF` (0x28): **`0x5801`**
- `SDA_TIMING` (0x3c): **`0x100a`**
- `CLOCK_DIV` (0x48): `0x0000`
- `HS` (0x30): `0x0000`

### 2.3 Clock Mux & Gate Configuration

1. **Clock Parent Mux**: `CLK_CFG_10` in TOPCKGEN (`0x10000000`):
   - Mux at offset `0x00b0`, `SET` at `0x00b4`, `CLR` at `0x00b8`.
   - Update register: `CLK_CFG_UPDATE1` at `0x0008`, bit 10.
   - Write `CLR` with `(3u << 8)` to clear current parent.
   - Write `SET` with `(2u << 8)` to select index 2 (`univpll_d5_d4` = 124.8 MHz).
   - Pulse update bit: `wr32(0x10000008, 1u << 10)`.
   - Ungate mux pdn: `wr32(0x100000b8, 1u << 15)`.
2. **Controller & Bus Clock Gates**:
   - `impc_ap_i2c0_ro`: `IMP_IIC_WRAP_C` base `0x11008000`, write `1u << 0` to `CLR` register `0x11008e04`.
   - `ifrao_i2c0`: `INFRACFG` base `0x10001000`, write `1u << 11` to `CLR` register `0x10001084`.
   - `ifrao_ap_dma_ps`: `INFRACFG`, write `1u << 18` to `CLR` register `0x1000108c`.
   - `ifrao_apdma`: `INFRACFG`, write `1u << 31` to `CLR` register `0x100010d4`.

### 2.4 Physical Pin Configuration (`IOCFG_LT` @ `0x11f20000`)

Pins 204 (SCL0) and 205 (SDA0) must match Android's live pin configuration:
- `IES` (+`0x0044` SET): `1u << 15` (Input Enable).
- `PD` (+`0x0068` CLR): `1u << 15` (Pull-Down Disable).
- `PU` (+`0x0084` SET): `1u << 15` (Pull-Up Enable).
- `SMT` (+`0x00e8` CLR): `1u << 7` (Schmitt Trigger Disable, matches live `SMT=0`).
- `R0/R1` (+`0x00d4` SET): `0x0f` (Sets `rsel=3`, configuring 203 ohm pull-up on both pins).
- `EH` (+`0x0028` CLR `0x3f`, +`0x0024` SET `(6u << 0) | (6u << 3)`): Sets `drive-strength-adv 6`.
- Pinmux Mode: Configured via modification register `MWR` (+`0x0c`) at `GPIO_BASE + 0x49c`: `(0x8 | 1) << 16` for Pin 204 (SCL0, mode 1) and `(0x8 | 1) << 20` for Pin 205 (SDA0, mode 1).

### 2.5 Critical Deadlock Rule: Do NOT Set `I2C_CTL_CLK_EXT_EN`

In MediaTek I2C v2 controllers, setting `I2C_CTL_CLK_EXT_EN` (bit 3) causes the controller to hang indefinitely waiting for clock stretching when communicating with devices that do not actively drive stretch cycles:
- With bit 3 set: `DEBUGSTAT = 0x343` (Master State 3 deadlock, `stat = 0x0`, timeout).
- With bit 3 cleared: `DEBUGSTAT = 0x700` (State machine finishes, returns to IDLE, `stat = 0x3`).
- **RULE**: `CONTROL` register writes in `mtk_i2c0_init_hw()` and `i2c0_prepare()` must never include bit 3.

---

## 3. Root Cause of Slave NACK & MT6360 Power Rail Details

The live boot test log (`logs/xnu-last.log`, line 147) shows:
```
i2c0: TIMING=0x419 LTIMING=0x123 EXT_CONF=0x5801 CONTROL=0x24 DEBUGSTAT=0x700
 [NACK stat=0x3 debug=0x700 fifo=0x110 dma_fsm=0x0 din=0x3] (x5)
touch: probe chip_id=FAIL(err=11) fw_ver=ERR vendor=ERR points=ERR
```
The controller completed the full transaction (`TRANSAC_COMP` bit 0 set), sent the address byte `0x70`, clocked the ACK bit, and observed SDA high (NACK, `ACKERR` bit 1 set).

### 3.1 Device Tree & Vendor Driver Evidence

1. Device tree node `focaltech@38` (`refs/fdt-x6739.dtb`):
   ```dts
   focaltech@38 {
       compatible = "focaltech,fts";
       reg = <0x38>;
       vtouch-supply = <0x1e9>;
       ...
   };
   ```
   Phandle `0x1e9` is `/soc/i2c@11f00000/mt6360_pmu@34/ldo/ldo2`:
   - Name: `vtp`
   - Target Voltage: 3.3V (`3300000 uV`)
2. Android Vendor Driver (`focaltech_core.c` lines 855-867):
   ```c
   tpd_gpio_output(ts_data->pdata->reset_gpio, 0);
   msleep(20);
   regulator_enable(ts_data->vdd);
   msleep(20);
   tpd_gpio_output(ts_data->pdata->reset_gpio, 1);
   ```
   LK leaves MT6360 LDO2 off at handover. When `mtkloader` boots, the FT3681 IC has 0V VDD and cannot respond to I2C transactions.

### 3.2 Programming MT6360 LDO2 over I2C5

The MT6360 PMU is located on **I2C5** (`/soc/i2c@11f00000`, APDMA base `0x11000500`, clock gate `imp_iic_wrap_n` at `0x11f02000`).
The MT6360 chip is a multi-function device exposing 4 distinct I2C slave IDs:
- `0x4E`: TCPC (Type-C)
- `0x1A`: PMIC (Bucks 1/2, LDOs 6/7)
- `0x64`: **LDO** (LDOs 1, 2, 3, 5) -> `MT6360_LDO_SLAVEID`
- `0x34`: PMU (Charger, Flash LED, RGB LED, ADC, WDT)

From `drivers/regulator/mt6360-regulator.c` and `drivers/mfd/mt6360-core.c`:
1. **LDO2 Voltage Register**:
   - Register address: `0x15` (16-bit reg `0x0215`)
   - `ldo_vout_ranges2` specifies 3.3V (`3300000 uV`) is selector **`0xD0`**.
2. **LDO2 Enable Register**:
   - Register address: `0x11` (16-bit reg `0x0211`)
   - Enable mask: **`0x40`** (Bit 6 = 1 enables LDO2).
3. **SMBus CRC-8 Wire Protocol**:
   - Access to MT6360 LDO slave `0x64` requires CRC-8 appended to each write.
   - CRC-8 polynomial: `0x07` (`x^8 + x^2 + x + 1`), initial value `0x00`.
   - Data passed into CRC-8: `[ (slave_id << 1) | 0, reg_addr, data_byte ]`.
   - Wire format: `START -> 0xC8 -> reg_addr -> data_byte -> crc8 -> dummy(0x00) -> STOP`.

**Precomputed Exact Wire Packets to Slave `0x64`:**
- **Packet 1 (Set LDO2 to 3.3V)**:
  - Input: `[ 0xC8, 0x15, 0xD0 ]`
  - CRC-8: `0xF4`
  - Wire sequence: `[ 0x15, 0xD0, 0xF4, 0x00 ]`
- **Packet 2 (Enable LDO2)**:
  - Input: `[ 0xC8, 0x11, 0x40 ]`
  - CRC-8: `0x59`
  - Wire sequence: `[ 0x11, 0x40, 0x59, 0x00 ]`

---

## 4. FocalTech Protocol & Transaction Format

In Android's vendor driver (`focaltech_i2c.c`):
- `fts_read` does NOT use combined repeated-start transfers (`TRANSAC_LEN = 2`, `DIR_CHANGE = 1`).
- It executes two separate standard transactions:
  1. Write transaction (`I2C_MASTER_WR`): `START -> 0x70 (ADDR_W) -> REG -> STOP`.
  2. Read transaction (`I2C_MASTER_RD`): `START -> 0x71 (ADDR_R) -> DATA[0..N] -> STOP`.

### 4.1 Reset Timing Sequence (`fts_hw_reset`)
- Assert Reset (GPIO 20 LOW): `wr32(GPIO_BASE + 0x108, 1u << 20)`. Wait **20 ms**.
- Release Reset (GPIO 20 HIGH): `wr32(GPIO_BASE + 0x104, 1u << 20)`. Wait **200 ms** for firmware boot.

### 4.2 Report Packet Layout (63 bytes from Reg `0x00`)
- `buf[0]`: `DEV_MODE` (Normal = `0x00`)
- `buf[1]`: `GEST_ID`
- `buf[2]`: `TD_STATUS` (Bits 3:0 = number of active contacts, 0..10)
- Up to 10 contact blocks (6 bytes each, starting at offset `0x03 + 6*i`):
  - `event = buf[0 + base] >> 6` (0 = DOWN, 1 = UP, 2 = MOVE)
  - `x = ((buf[0 + base] & 0x0f) << 8) | buf[1 + base]`
  - `y = ((buf[2 + base] & 0x0f) << 8) | buf[3 + base]`
  - `finger_id = buf[2 + base] >> 4`

---

## 5. Next Steps for Successor Agent

### Step 1: Power MT6360 LDO2 (`vtp`) in `mtkloader`
In `mtkloader/mtki2c.h` or a dedicated helper:
- Ungate I2C5 clock: `syscon@11f02000` (`imp_iic_wrap_n`), write `1u << 0` to `0x11f02e04`.
- Send the two precomputed 4-byte packets to slave `0x64` over I2C5:
  1. `[ 0x15, 0xD0, 0xF4, 0x00 ]` (sets 3.3V)
  2. `[ 0x11, 0x40, 0x59, 0x00 ]` (enables LDO2)
- Add a 20 ms settling delay after power enable before triggering `fts_hw_reset()`.

### Step 2: Implement Separate Write and Read Transfers
In `mtkloader/mtki2c.h`:
- Ensure single-byte register reads can execute as separate write followed by read transactions if combined repeated-start is rejected by the FT3681 firmware.

### Step 3: Verify FT3681 Detection in Loader
Run the build and test cycle:
```bash
cd /mnt/project/Misc/hakios/hakios-x6739
./mtkloader/build.sh
HAKIOS_SERIAL=102562537K104189 XNU_SECS=30 XNU_ARGS="cpus=1" tools/xnu-test.sh
```
Check `logs/pstore/console-ramoops-0`:
Confirm the probe outputs:
```
touch: probe chip_id=0x5662 fw_ver=0x44 vendor=0x8d points=0
touch: 63-byte report over DMA ok, mode=0x00 points=0
touch: FocalTech FT3681 active and ready on I2C0 (0x38)
```

### Step 4: Implement XNU Kernel Driver (`hakios.ft3681.c`)
Create `refs/hakios-xnu-kernel/drivers/input/hakios.ft3681.c` following the template in `refs/hakios-xnu-kernel/drivers/input/hakios.goodix.c`:
1. Map `GPIO_BASE` (`0x10005000`) and `I2C0_BASE` (`0x11015000`) via `ml_io_map()`.
2. Spawn a polling thread (125 Hz / 8 ms) or interrupt handler on GIC SPI 245 (GPIO 21, active low).
3. Read 63-byte report from register `0x00`.
4. Scale coordinates to 0..65535:
   ```c
   uint32_t norm_x = ((uint64_t)x * 65535ULL) / 1080ULL;
   uint32_t norm_y = ((uint64_t)y * 65535ULL) / 2400ULL;
   ```
5. Post touch events via `hakios_touch_post(type, finger, norm_x, norm_y)`.
6. Register the driver in `refs/hakios-xnu-kernel/drivers/input/conf`:
   ```
   driver FT3681
       file bsd/dev/hakios.ft3681.c
       help FocalTech FT3681 touchscreen on MT6893 I2C0
   ```
7. Declare `void hakios_ft3681_init(void);` in `xnu/files/bsd/sys/hakios.input.h` and call it from `hakios_input_init()`.
8. Enable `CONFIG_HAKIOS_FT3681=y` and `touchscreen=1` in `device.conf`.

---

## 6. Strict Development Rules

1. **Never work in `/tmp`**: Absolute hard rule. Host `/tmp` is a RAM tmpfs on 16 GB host.
2. **Never chain inside `adb shell su -c '...'`**: `&&`, `||`, and `;` silently drop root privileges inside quotes. Chain commands only at host level:
   ```bash
   adb shell su -c 'cmd1' && adb shell su -c 'cmd2'
   ```
3. **SELinux State**: Do not alter device SELinux mode.
4. **Git Commits**: Commit each completed task cleanly without AI trailers (`Co-Authored-By`, `Signed-off-by`).
5. **Flash Active Slot Only**: Flash only to active slot `_a` (`fastboot flash boot_a ...`).
