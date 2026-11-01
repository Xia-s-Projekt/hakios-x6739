# FocalTech FT3681 Touchscreen Bring-Up (Infinix GT 10 Pro / X6739 - MT6893)

## 1. Hardware Architecture & Identity

The touchscreen controller on the Infinix GT 10 Pro (X6739, MediaTek MT6893 Dimensity 8050) is a **FocalTech FT3681** communicating over **I2C0** (not SPI, not TCM).

### 1.1 Verified Hardware Identifiers

All values verified against device tree (`refs/fdt-x6739.dtb`), live Android sysfs (`/sys/devices/platform/soc/11015000.i2c/i2c-0/0-0038/`), and kernel driver (`refs/focaltech_ft3681.ko`):

| Parameter | Value | Source / Verification |
|---|---|---|
| Controller IC | FocalTech FT3681 (`TM_FT3681_00`) | `/proc/device-tree/touch/supplier-0` |
| Chip ID High | `0x56` (Register `0xA3`) | Android `fts_driver_info` (`IC ID: 0x5662`) |
| Chip ID Low | `0x62` (Register `0x9F`) | Android `fts_driver_info` |
| Firmware Version | `0x44` (Register `0xA6`) | Android `tp_firmware_version` (`0x44,bv:0x44`) |
| Vendor ID | `0x8D` (Register `0xA8`) | Android `fts_dump_reg` (`Vendor ID:0x8d`) |
| Bus Interface | I2C0 (`/soc/i2c@11015000`) | Device tree `/soc/i2c@11015000/focaltech@38` |
| 7-bit Slave Address | `0x38` (8-bit write `0x70`, read `0x71`) | Device tree `reg = <0x38>` |
| Bus Frequency | 400 kHz (Fast Mode) | Device tree `clock-frequency = <0x061a80>` |
| Power Supply | MT6360 PMIC LDO2 (`vtp`) | Device tree `vtouch-supply = <&mt6360_ldo2>` (default on in LK) |
| Max Touch Points | 10 contacts | Device tree `focaltech,max-touch-number = <10>` |
| Touch Coordinates | 1080 x 2400 (Native) | DT `focaltech,display-coords = <0 0 17279 38399>` (16x subpixels) |

---

## 2. Pinmux & GPIO Mapping

Verified against MT6885/MT6893 pinctrl module (`pinctrl-mt6885.ko` relocations and symbol ranges):

| Function | Pin Number | Pinmux Register | Bitfield Offset | Target Mode | Notes |
|---|---|---|---|---|---|
| I2C0 SCL | GPIO 204 | `GPIO_BASE + 0x490`, written through MWR at `+ 0x49c` | Bits 16..19 | Mode 1 (`SCL0`) | Mode 0 = `GPIO204`; the mode register's MSB is the modification-enable bit, so the write is `(0x8 \| 1) << 16` |
| I2C0 SDA | GPIO 205 | `GPIO_BASE + 0x490`, written through MWR at `+ 0x49c` | Bits 20..23 | Mode 1 (`SDA0`) | Mode 0 = `GPIO205`; the write is `(0x8 \| 1) << 20` |
| FT3681 Reset | GPIO 20 | `GPIO_BASE + 0x320` | Bits 16..19 | Mode 0 (`GPIO20`) | Active-low; pull low 20 ms, then high 200 ms |
| FT3681 IRQ | GPIO 21 | `GPIO_BASE + 0x320` | Bits 20..23 | Mode 0 (`GPIO21`) | Active-low; low indicates touch report pending |

The pull-ups for pins 204/205 live in `iocfg_lt` (`0x11f20000`): IES at `+0x040` bit 15,
pull-up at `+0x080` bit 15, pull-down at `+0x060` bit 15, schmitt at `+0x00e0` bit 7. The phone's
own live pinconf (`logs/pinconf-pins.txt`) shows pins 204/205 with a 203-ohm pull-up, input
enabled and schmitt enabled.

### 2.1 Pinctrl Base and Register Offsets

- `GPIO_BASE`: `0x10005000` (iocfg0).
- Direction Registers:
  - Pins 0..31: `0x000` (Set `0x004`, Clear `0x008`). Pin 20 = bit 20.
  - Pins 192..219: `0x060` (Set `0x064`, Clear `0x068`). Pin 204 = bit 12, Pin 205 = bit 13.
- Data Output Registers (DOUT):
  - Pins 0..31: `0x100` (Set `0x104`, Clear `0x108`). Pin 20 = bit 20.
  - Pins 192..219: `0x160` (Set `0x164`, Clear `0x168`). Pin 204 = bit 12, Pin 205 = bit 13.
- Data Input Registers (DIN):
  - Pins 0..31: `0x200`. Pin 21 = bit 21.
  - Pins 192..219: `0x260`. Pin 204 = bit 12, Pin 205 = bit 13.
- Input Enable Status (IES):
  - Pins 204..205: Base `0x11f20000` (iocfg_lt), offset `0x040`.

---

## 3. I2C0 Controller Architecture & Clocks

MediaTek MT6893 I2C controllers use 16-bit register widths. Using 32-bit writes corrupts adjacent registers.

### 3.1 Register Map (`0x11015000`, 16-bit access)

This is the **v2** table: the phone's own `i2c-mt65xx.ko` carries `mt6873_compat` (and no
`mt6893`/`mt6885` profile), and in Linux 6.1 `mt6873_compat` uses `mt_i2c_regs_v2` with
`slave_addr_ver` **unset**, so the slave address goes to `0x04`, never to `0x94`. Read it out of
the module with `tools/i2c-vendor-module.py`, which prints the profile's flags as well.

```
0x00: OFFSET_DATA_PORT
0x04: OFFSET_SLAVE_ADDR      (7-bit addr << 1)   <- the one this SoC uses
0x08: OFFSET_INTR_MASK
0x0c: OFFSET_INTR_STAT        (Clear by writing active bits)
0x10: OFFSET_CONTROL          (RS, DMA_EN, CLK_EXT_EN, DIR_CHANGE, ACKERR_DET_EN)
0x14: OFFSET_TRANSFER_LEN     (Bytes for write phase)
0x18: OFFSET_TRANSAC_LEN      (Transaction count: 2 for WRRD)
0x1c: OFFSET_DELAY_LEN
0x20: OFFSET_TIMING           (0x0419 for 400 kHz off 124.8 MHz)
0x24: OFFSET_START            (0x0001: I2C_TRANSAC_START)
0x28: OFFSET_EXT_CONF         (0x5801: the computed start/stop timing)
0x2c: OFFSET_LTIMING          (0x0123: low timing for SCL)
0x30: OFFSET_HS               (High speed config; 0 for fast mode)
0x34: OFFSET_IO_CONFIG        (0x0003: Open-drain)
0x38: OFFSET_FIFO_ADDR_CLR    (0x0001)
0x3c: OFFSET_SDA_TIMING       (0x100a)
0x40: OFFSET_MCU_INTR
0x44: OFFSET_TRANSFER_LEN_AUX (Bytes for read phase in WRRD)
0x48: OFFSET_CLOCK_DIV        (0x0000)
0x50: OFFSET_SOFTRESET        (0x0001)
0x70: OFFSET_HFIFO_DATA
0x8c: OFFSET_MULTI_DMA
0x90: OFFSET_SCL_MIS_COMP_POINT (0x0000)
0x94: OFFSET_SLAVE_ADDR1      (v3 only: NOT used on this SoC)
0xe4: OFFSET_DEBUGSTAT        (State machine & line monitor)
0xe8: OFFSET_DEBUGCTRL
0xec: OFFSET_DMA_FSM_DEBUG
0xf4: OFFSET_FIFO_STAT
0xf8: OFFSET_FIFO_THRESH
0xf88: OFFSET_DCM_EN
```

The six timing values are not guessed: `tools/i2c-timing.py` is a port of the 6.1
`mtk_i2c_set_speed` path for `mt6873_compat`, and for a 124.8 MHz parent at 400 kHz it gives
TIMING 0x0419, HS 0x0000, LTIMING 0x0123, EXT_CONF 0x5801, CLOCK_DIV 0x0000, SDA_TIMING 0x100a.

### 3.2 The order the registers are written in

`mtk_i2c_init_hw` (6.1), for this SoC's profile:

1. START = 0; read INTR_STAT, write it back to clear.
2. APDMA: WARM_RST → CLR → HANDSHAKE_RST|HARD_RST, and SOFTRESET = HANDSHAKE_RST|SOFT;
   then CLR again and SOFTRESET = 0.
3. IO_CONFIG = 0x0003 (open drain).
4. DCM_EN (only if the profile has `dcm`; mt6873 does not).
5. TIMING (only if the profile has no `speed_ver`; mt6873 does not), then HS, then LTIMING.
6. `timing_adjust`: EXT_CONF, CLOCK_DIV, SCL_MIS_COMP_POINT = 0, SDA_TIMING, then TIMING, HS and
   LTIMING again.
7. CONTROL = ACKERR_DET_EN | CLK_EXT_EN | DMA_EN | DMAACK_EN | ASYNC_MODE.
8. DELAY_LEN = 2.

`mtk_i2c_do_transfer` (6.1), for a write-then-read (WRRD):

CONTROL → SLAVE_ADDR (0x04) → INTR_STAT (write 1 to clear) → FIFO_ADDR_CLR (0x0001) → INTR_MASK
→ TRANSFER_LEN / TRANSFER_LEN_AUX / TRANSAC_LEN (0x0002) → APDMA (INT_FLAG, CON, TX/RX MEM_ADDR,
TX/RX_LEN, 4G_MODE) → memory barrier → DMA EN → START.

### 3.2 APDMA Engine (`0x11000080`, 32-bit access)

```
0x00: APDMA_REG_INT_FLAG
0x04: APDMA_REG_INT_EN
0x08: APDMA_REG_EN            (0x0001: Start DMA)
0x0c: APDMA_REG_RST           (Warm reset 0x01, Hard reset 0x02)
0x18: APDMA_REG_CON           (Async mode, dir change, skip config)
0x1c: APDMA_REG_TX_MEM_ADDR   (Physical address of TX buffer)
0x20: APDMA_REG_RX_MEM_ADDR   (Physical address of RX buffer)
0x24: APDMA_REG_TX_LEN
0x28: APDMA_REG_RX_LEN
0x54: APDMA_REG_TX_4G_MODE    (0 for 32-bit PA)
0x58: APDMA_REG_RX_4G_MODE    (0 for 32-bit PA)
```

### 3.3 Clocks & Gating

The phone's own clock dump (`logs/clk_summary.txt`) says `i2c_sel` = **124800000 Hz**, `i2c_ck`
= 124800000 and `impc_ap_i2c0_ro` = 124800000. So the parent is already the right one; the
loader still has to set it explicitly, because it runs before the phone's own kernel and cannot
count on the preloader having left it.

- **Parent Clock**: `i2c_sel`, a TOPCKGEN mux at `0x10000000`.
  - `CLK_CFG_10 = 0x00b0`, `CLK_CFG_10_SET = 0x00b4`, `CLK_CFG_10_CLR = 0x00b8`,
    `CLK_CFG_UPDATE1 = 0x0008`, `TOP_MUX_I2C_SHIFT = 10`; the field is at lsb 8, width 2, and
    the mux's own gate (pdn) is bit 15.
  - The parents are `tck_26m_mx9_ck`, `mainpll_d4_d8`, `univpll_d5_d4` — **index 2 =
    univpll_d5_d4 = 124.8 MHz**.
  - How MediaTek's clock driver writes it (`clk-mux.c`): write CLR with the mask
    (`3 << 8`), then SET with the index (`2 << 8`), then pulse the update bit (`BIT(10)` to
    `0x008`). To ungate the mux, write CLR (`0xb8`) with `BIT(15)` — the pdn bit clear means
    enabled.
- **Controller Clock**: `impc_ap_i2c0_ro` (bit 0) from `IMP_IIC_WRAP_C` (`0x11008000`).
  - Status: `0x11008000 + 0xe00` (`impc_cg_regs.sta_ofs`)
  - Clear Gate (Enable): `0x11008000 + 0xe04` (`impc_cg_regs.clr_ofs`)
  - Set Gate (Disable): `0x11008000 + 0xe08` (`impc_cg_regs.set_ofs`)
- **The other three gates** the I2C0 path needs, in INFRACFG (`0x10001000`), each written to its
  clear register with its shift bit:
  - `ifrao_i2c0` (shift 11) → clr `0x84`
  - `ifrao_ap_dma_ps` (shift 18) → clr `0x8c`
  - `ifrao_apdma` (shift 31) → clr `0xd4`
- **DMA Clock**: `CLK_INFRA_AP_DMA` in INFRACFG (`0x10001000 + 0x0d4`, bit 31).

Writing `0xffffffff` to a clear register opens *every* gate in that block, which is what the
loader currently does; each gate should be opened one at a time.

### 3.4 State Machine Diagnosis: From Deadlock (`0x343`) to Active Bus Completion (`0x700`)

In earlier builds, the hardware controller stalled in state 3:
- Register dump `DEBUGSTAT = 0x343`:
  - Bits `4:0` = `0x03` (`MASTER_STATE = 3`): "Preparing to transmit a Data bit (`SCL=0`, `SDA=data`)".
  - Bit `6` = `1` (`MASTER_WRITE = 1`).
  - Bit `7` = `0` (`BUS_BUSY = 0`).
- Root cause: Setting `I2C_CTL_CLK_EXT_EN` (bit 3) in `CONTROL` caused the controller to wait indefinitely for clock-stretching that never arrived, or clock gates were shut (`WRAP_C_STA = 0`).

**The Fix and Verification:**
1. Dropping `I2C_CTL_CLK_EXT_EN` from `CONTROL` in `mtk_i2c0_init_hw()` and `i2c0_prepare()` completely eliminated the State 3 hang.
2. The clock gates are properly un-gated (`CLK_CFG_10` mux index 2 = 124.8 MHz `univpll_d5_d4`, `impc_ap_i2c0_ro` at `0x11008000 + 0xe04` bit 0, `ifrao_i2c0` bit 11, `ifrao_ap_dma_ps` bit 18, `ifrao_apdma` bit 31).
3. Live test console (`logs/xnu-last.log`, line 147):
   ```
   i2c0: TIMING=0x419 LTIMING=0x123 EXT_CONF=0x5801 CONTROL=0x24 DEBUGSTAT=0x700
    [NACK stat=0x3 debug=0x700 fifo=0x110 dma_fsm=0x0 din=0x3] (x5)
   ```
   `DEBUGSTAT = 0x700` proves the controller is in `MASTER_STATE = 0` (IDLE), bus not busy, and completed the full transaction.
   `stat = 0x3` = `I2C_STAT_TRANSAC_COMP` (0x01) | `I2C_STAT_ACKERR` (0x02).
   The hardware controller successfully issues START, transmits the 7-bit slave address `0x38`, clocks the 8th bit, observes that the slave does not pull SDA low for ACK, completes the cycle, and returns to IDLE.

### 3.5 Physical Pinconf & Drive Strength (`IOCFG_LT` @ `0x11f20000`)

Pins 204 (SCL0) and 205 (SDA0) belong to `IOCFG_LT` (base `0x11f20000`). The phone's live Android configuration was captured and applied in `mtki2c.h` `i2c0_pin_pull_up()`:
- `IES` (+`0x0044` SET): bit 15 set (Input Enable).
- `PD` (+`0x0068` CLR): bit 15 cleared (Pull-Down disabled).
- `PU` (+`0x0084` SET): bit 15 set (Pull-Up enabled).
- `SMT` (+`0x00e8` CLR): bit 7 cleared (Schmitt Trigger disabled, matching live Android state `SMT=0`).
- `R0/R1` (+`0x00d4` SET): write `0x0f` (sets `rsel=3`, corresponding to 203 ohm pull-up on both pins 204 & 205).
- `DRV` (+`0x0004` SET): bits 18..23.
- `EH` (+`0x0028` CLR `0x3f`, +`0x0024` SET `(6u << 0) | (6u << 3)`): sets `drive-strength-adv 6`.
- Pinmux Mode: Configured via modification-enable write register `MWR` (+`0x0c`) at `GPIO_BASE + 0x49c`: `(0x8 | 1) << 16` for Pin 204 (mode 1, SCL0) and `(0x8 | 1) << 20` for Pin 205 (mode 1, SDA0).

### 3.6 Root Cause of NACK: Power Supply Architecture (`vtouch-supply` / MT6360 `vtp`)

The FocalTech FT3681 is NACKing slave address `0x38` because the touch IC is **unpowered**:
- In the device tree (`fdt-x6739.dtb`):
  ```dts
  focaltech@38 {
      compatible = "focaltech,fts";
      reg = <0x38>;
      vtouch-supply = <0x1e9>;
      ...
  };
  ```
  Phandle `0x1e9` resolves directly to `/soc/i2c@11f00000/mt6360_pmu@34/ldo/ldo2`:
  - Regulator Name: `vtp`
  - Target Voltage: 3.3V (`3300000 uV`)
- In Android's vendor touch driver (`focaltech_core.c`, lines 855-867):
  ```c
  tpd_gpio_output(ts_data->pdata->reset_gpio, 0);
  msleep(20);
  regulator_enable(ts_data->vdd);
  msleep(20);
  tpd_gpio_output(ts_data->pdata->reset_gpio, 1);
  ```
  Android explicitly enables this regulator on kernel driver boot.
- When booting our bare-metal loader from LK/fastboot, LK leaves MT6360 LDO2 (`vtp`) disabled or unpowered. Without 3.3V VDD, the FT3681 IC cannot respond to I2C transfers.

### 3.7 Programming MT6360 LDO2 (`vtp`) over I2C5

The MT6360 PMU is located on **I2C5** (`/soc/i2c@11f00000`, APDMA `0x11000500`, clock gate `imp_iic_wrap_n` at `0x11f02000`).
The MT6360 chip is a multi-function device exposing 4 distinct I2C slave IDs:
- `0x4E`: TCPC (Type-C)
- `0x1A`: PMIC (Bucks 1/2, LDOs 6/7)
- `0x64`: **LDO** (LDOs 1, 2, 3, 5) -> `MT6360_LDO_SLAVEID`
- `0x34`: PMU (Charger, Flash LED, RGB LED, ADC, WDT)

From `drivers/regulator/mt6360-regulator.c` and `drivers/mfd/mt6360-core.c`:
1. **LDO2 Voltage Setting**:
   - Register: `0x15` (16-bit reg `0x0215`)
   - Voltage Range: `ldo_vout_ranges2` specifies 3.3V is selector `0xD0` (`3300000 uV`).
2. **LDO2 Enable**:
   - Register: `0x11` (16-bit reg `0x0211`)
   - Enable Mask: `0x40` (Bit 6 = 1 enables LDO2).
3. **SMBus CRC-8 Wire Protocol**:
   - PMIC and LDO slave registers on MT6360 mandate CRC-8 verification.
   - CRC-8 polynomial: `0x07` (`x^8 + x^2 + x + 1`), initial value `0x00`.
   - Data passed into CRC-8: `[ (slave_id << 1) | 0, reg_addr, data_byte ]`.
   - Wire format: `START -> (slave_id << 1) -> reg_addr -> data_byte -> crc8 -> dummy(0x00) -> STOP`.
   - Wire Packet 1 (Set 3.3V on LDO2):
     - Slave: `0x64`
     - Header byte: `(0x64 << 1) = 0xC8`
     - Input to CRC: `[ 0xC8, 0x15, 0xD0 ]` -> CRC = `0xF4`
     - Wire payload: `0x15, 0xD0, 0xF4, 0x00`
   - Wire Packet 2 (Enable LDO2):
     - Slave: `0x64`
     - Header byte: `0xC8`
     - Input to CRC: `[ 0xC8, 0x11, 0x40 ]` -> CRC = `0x59`
     - Wire payload: `0x11, 0x40, 0x59, 0x00`

---

## 4. FocalTech FT3681 Protocol & Report Parsing

### 4.1 Register Layout

- `0x00`: `DEV_MODE` (Normal = `0x00`)
- `0x01`: `GEST_ID` (Gesture identifier)
- `0x02`: `TD_STATUS` Multi-touch status:
  - Bits `3:0`: Number of active touch points (0..10).
- `0x03..0x3E`: Coordinate blocks (6 bytes per contact, up to 10 contacts):
  - Offset `+0` (`0x03 + 6*i`):
    - Bits `7:6`: Event flag:
      - `0x00`: `DOWN` (landed)
      - `0x40`: `UP` (lifted)
      - `0x80`: `CONTACT` / `MOVE` (active contact)
      - `0xC0`: Reserved
    - Bits `3:0`: X position bits `11:8`
  - Offset `+1` (`0x04 + 6*i`): X position bits `7:0`
    - `X = ((buf[0] & 0x0F) << 8) | buf[1]`
  - Offset `+2` (`0x05 + 6*i`):
    - Bits `7:4`: Touch ID (finger ID, 0..9)
    - Bits `3:0`: Y position bits `11:8`
  - Offset `+3` (`0x06 + 6*i`): Y position bits `7:0`
    - `Y = ((buf[2] & 0x0F) << 8) | buf[3]`
  - Offset `+4` (`0x07 + 6*i`): Touch weight
  - Offset `+5` (`0x08 + 6*i`): Area / misc

### 4.2 Transaction Protocol: Separate Write and Read Transactions

In the vendor kernel driver (`focaltech_i2c.c`, `fts_read`):
```c
int fts_read(struct i2c_client *client, u8 *cmd, u32 cmdlen, u8 *data, u32 datalen)
{
    ...
    ret = i2c_master_send(client, cmd, cmdlen);
    ...
    ret = i2c_master_recv(client, data, datalen);
    ...
}
```
FocalTech FT3681 expects two distinct I2C bus transactions:
1. `i2c_master_send`: `START -> 0x70 (ADDR_W) -> REG -> STOP`.
2. `i2c_master_recv`: `START -> 0x71 (ADDR_R) -> DATA[0..N] -> STOP`.

Combined repeated-start transfers (`TRANSAC_LEN = 2`, `DIR_CHANGE = 1`) may not be supported by the FT3681 firmware in standard report polling mode.

---

## 5. Kernel Driver Architecture for XNU

The touchscreen communicates with iOS userland through the `/dev/hakios-touch` character device managed by `bsd/dev/hakios.input.c`.

### 5.1 Pipeline Flow

```
+-------------------------------------------------------------+
| Physical Hardware: FT3681 Touch Controller                  |
| I2C0 (0x11015000), Slave 0x38, INT GPIO 21, Reset GPIO 20   |
| Power: MT6360 LDO2 (vtp, 3.3V) via I2C5 (0x11f00000)        |
+-------------------------------------------------------------+
                              |
                              v [I2C0 Polling / GPIO 21 IRQ]
+-------------------------------------------------------------+
| XNU Kernel Driver: bsd/dev/hakios.ft3681.c                  |
| 1. Checks GPIO 21 (asserted low = touch data ready)         |
| 2. Reads 63 bytes (0x00..0x3E) from FT3681 over I2C0        |
| 3. Unpacks contacts (event flag, finger ID, X, Y)           |
| 4. Normalizes coordinates to 0..65535:                      |
|    nx = (x * 65535) / 1080;  ny = (y * 65535) / 2400;       |
| 5. Calls hakios_touch_post(type, finger, nx, ny)            |
+-------------------------------------------------------------+
                              |
                              v [Ring Buffer in hakios.input.c]
+-------------------------------------------------------------+
| /dev/hakios-touch (Char Device 0666)                        |
| struct hakios_touch { time, type, finger, x, y }            |
+-------------------------------------------------------------+
                              |
                              v [Read by libhakios.dylib]
+-------------------------------------------------------------+
| iOS 16.5 backboardd (Window Server / HID Event Dispatcher)  |
| send_touch() synthesizes IOHIDEvent digitizer packets       |
| -> SpringBoard processes touch, unlock, app launching       |
+-------------------------------------------------------------+
```

### 5.2 Coordinate Normalization Formula

Userland `backboardd` (`inject.c` lines 822-823) interprets coordinates as:
```c
fingers[f].x = t->x / 65535.0;
fingers[f].y = t->y / 65535.0;
```
Therefore, the kernel touch driver must scale raw panel coordinates (0..1080, 0..2400) into `0..65535`:
```c
uint32_t norm_x = ((uint64_t)raw_x * 65535ULL) / 1080ULL;
uint32_t norm_y = ((uint64_t)raw_y * 65535ULL) / 2400ULL;
```

---

## 6. Implementation Checklist for Next Agent

1. **Power up MT6360 LDO2 (`vtp`) before touch reset and probe**:
   - Program I2C5 (`0x11f00000`, clock gate `imp_iic_wrap_n` `0x11f02000 + 0xe04` bit 0) or send the 2 packets to slave `0x64`:
     - Packet 1 (3.3V): `[0x15, 0xD0, 0xF4, 0x00]`
     - Packet 2 (Enable): `[0x11, 0x40, 0x59, 0x00]`
   - Delay 20 ms for regulator voltage settling before asserting reset.
2. **Execute Separate Write and Read Transfers**:
   - In `mtki2c.h`: Ensure write-then-read can be performed as `I2C_MASTER_WR` (TRANSAC_LEN 1, TRANSFER_LEN tx_len) followed by `I2C_MASTER_RD` (TRANSAC_LEN 1, TRANSFER_LEN rx_len) if repeated start NACKs.
3. **Verify Loader Detection**:
   - Run `./mtkloader/build.sh` and test with `tools/xnu-test.sh`.
   - Confirm log prints:
     `touch: FocalTech FT3681 active and ready on I2C0 (0x38)`
     with Chip ID `0x5662`, Firmware `0x44`, Vendor `0x8D`.
4. **Create the XNU Driver (`hakios.ft3681.c`)**:
   - File: `kernel/bsd/dev/hakios.ft3681.c` in this repository, carried into the common tree (`refs/hakios-xnu-kernel/drivers/input/`).
   - Map `GPIO_BASE` (`0x10005000`) and `I2C0_BASE` (`0x11015000`) via `ml_io_map()`.
   - Read report: 63 bytes from register `0x00` in separate write-then-read transfers, 3 retries.
   - Polling thread at 8000 us (125 Hz) checking GPIO 21 (DIN bit 21 == 0), or an interrupt on GIC SPI 245.
   - Unpack the report and post `HAKIOS_TOUCH_DOWN`, `HAKIOS_TOUCH_MOVE`, `HAKIOS_TOUCH_UP` into `hakios_touch_post()`, with the coordinates scaled to 0..65535.
   - The template is the common tree's `drivers/input/hakios.goodix.c`, which already has a FocalTech FT3680 over SPI.
5. **Register the driver in the common tree**:
   - `drivers/input/conf`:
     ```
     driver FT3681
         file bsd/dev/hakios.ft3681.c
         help FocalTech FT3681 touchscreen on MT6893 I2C0
     ```
   - Add `void hakios_ft3681_init(void);` to `xnu/files/bsd/sys/hakios.input.h` and call it from
     `drivers/input/hakios.input.c`'s `hakios_input_init()`, next to `hakios_goodix_init()`.
   - Enable `CONFIG_HAKIOS_FT3681=y` and pass `touchscreen=1` in `XNU_ARGS`.
6. **Fix the PMIC wrapper** (`mtkloader/mtkpwrap.h`) — it is the MT6768 board's: the wrapper is at
   `0x10026000`, the PMIC is an MT6359P, and the mt6885 wrapper uses ARB_V3
   (SWINF_2_ACC 0xC80, WDATA 0xC84, RDATA 0xC94, VLD_CLR 0xCA4, STA 0xCA8, FSM = `(value >> 1) & 7`).
   Not needed for the touch, but it is used by the GPU probe and it is wrong.
