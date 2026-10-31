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
| I2C0 SCL | GPIO 204 | `GPIO_BASE + 0x490` | Bits 16..19 | Mode 1 (`SCL0`) | Mode 0 = `GPIO204` |
| I2C0 SDA | GPIO 205 | `GPIO_BASE + 0x490` | Bits 20..23 | Mode 1 (`SDA0`) | Mode 0 = `GPIO205` |
| FT3681 Reset | GPIO 20 | `GPIO_BASE + 0x320` | Bits 16..19 | Mode 0 (`GPIO20`) | Active-low; pull low 10ms, then high 200ms |
| FT3681 IRQ | GPIO 21 | `GPIO_BASE + 0x320` | Bits 20..23 | Mode 0 (`GPIO21`) | Active-low; low indicates touch report pending |

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

```
0x00: OFFSET_DATA_PORT
0x04: OFFSET_SLAVE_ADDR      (7-bit addr << 1)
0x08: OFFSET_INTR_MASK
0x0c: OFFSET_INTR_STAT        (Clear by writing active bits)
0x10: OFFSET_CONTROL          (RS, DMA_EN, CLK_EXT_EN, DIR_CHANGE, ACKERR_DET_EN)
0x14: OFFSET_TRANSFER_LEN     (Bytes for write phase)
0x18: OFFSET_TRANSAC_LEN      (Transaction count: 2 for WRRD)
0x1c: OFFSET_DELAY_LEN
0x20: OFFSET_TIMING           (High timing / clock stretch)
0x24: OFFSET_START            (0x0001: I2C_TRANSAC_START)
0x28: OFFSET_EXT_CONF         (0x1800 for Fast Mode)
0x2c: OFFSET_LTIMING          (Low timing for SCL)
0x30: OFFSET_HS               (High speed config)
0x34: OFFSET_IO_CONFIG        (0x0003: Open-drain)
0x38: OFFSET_FIFO_ADDR_CLR    (0x0001)
0x44: OFFSET_TRANSFER_LEN_AUX (Bytes for read phase in WRRD)
0x48: OFFSET_CLOCK_DIV        (0x0000)
0x50: OFFSET_SOFTRESET        (0x0001)
0xe4: OFFSET_DEBUGSTAT        (State machine & line monitor)
0xe8: OFFSET_DEBUGCTRL
```

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

- **Parent Clock**: `top_i2c` via TOPCKGEN (`0x10000000`). Un-gated in `CLK_CFG_10_CLR` (`0x0b8`, bit 15), latched with pulse to `CLK_CFG_UPDATE1` (`0x008`, bit 10).
- **Controller Clock**: `impc_ap_i2c0_ro` (bit 0) from `IMP_IIC_WRAP_C` (`0x11008000`).
  - Status: `0x11008000 + 0xe00` (`impc_cg_regs.sta_ofs`)
  - Clear Gate (Enable): `0x11008000 + 0xe04` (`impc_cg_regs.clr_ofs`)
  - Set Gate (Disable): `0x11008000 + 0xe08` (`impc_cg_regs.set_ofs`)
- **DMA Clock**: `CLK_INFRA_AP_DMA` in INFRACFG (`0x10001000 + 0x0d4`, bit 31).

### 3.4 State Machine Diagnosis (`DEBUGSTAT = 0x343`)

The register dump `DEBUGSTAT = 0x343` breaks down as:
- Bits `4:0` = `0x03` (`MASTER_STATE = 3`): "Preparing to transmit a Data bit (`SCL=0`, `SDA=data`)".
- Bit `6` = `1` (`MASTER_WRITE = 1`).
- Bit `7` = `0` (`BUS_BUSY = 0`).

State 3 stalls indefinitely when the I2C clock generator is not pulsing SCL (either input clock is inactive, or SCL line is held low by bus contention / lack of pull-up).

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

---

## 5. Kernel Driver Architecture for XNU

The touchscreen communicates with iOS userland through the `/dev/hakios-touch` character device managed by `bsd/dev/hakios.input.c`.

### 5.1 Pipeline Flow

```
+-------------------------------------------------------------+
| Physical Hardware: FT3681 Touch Controller                  |
| I2C0 (0x11015000), Slave 0x38, INT GPIO 21, Reset GPIO 20   |
+-------------------------------------------------------------+
                              |
                              v [I2C0 Polling / GPIO 21 IRQ]
+-------------------------------------------------------------+
| XNU Kernel Driver: bsd/dev/x6739_ft3681.c                   |
| 1. Checks GPIO 21 (asserted low = touch data ready)         |
| 2. Reads 32 bytes (0x00..0x1F) from FT3681 over I2C0        |
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

1. **Resolve I2C0 Bus Transfer in `mtkloader` / Kernel**:
   - Verify SCL and SDA line states via `rd32(GPIO_BASE + 0x260)` before initiating transaction.
   - Ensure `IMP_IIC_WRAP_C` clock is un-gated via `wr32(0x11008000 + 0xe04, 1u << 0)` and TOPCKGEN clock update bit is set.
   - If APDMA FSM continues to stall at state 3, use software bitbang on GPIO 204/205 (`i2c_bb_read_reg`) for deterministic bringup (identical to HakiOS camera and speaker bitbang drivers).
2. **Create Kernel Driver**:
   - File: `refs/hakios-xnu-kernel/drivers/input/hakios.ft3681.c` (or `kernel/bsd/dev/x6739_ft3681.c`).
   - Map `GPIO_BASE` (`0x10005000`) and `I2C0_BASE` (`0x11015000`) via `ml_io_map()`.
   - Polling thread at 8000us (125 Hz) checking GPIO 21 (DIN bit 21 == 0).
   - Unpack report and post `HAKIOS_TOUCH_DOWN`, `HAKIOS_TOUCH_MOVE`, `HAKIOS_TOUCH_UP` into `hakios_touch_post()`.
3. **Register Driver in Kernel Config**:
   - Add driver entry to `drivers/input/conf`:
     ```
     driver FT3681
         file bsd/dev/hakios.ft3681.c
         help FocalTech FT3681 touchscreen on MT6893 I2C0
     ```
   - Enable `CONFIG_HAKIOS_FT3681=y` and pass `touchscreen=1` in `XNU_ARGS`.
