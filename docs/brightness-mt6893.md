# Brightness on the X6739: how the value reaches the panel

Written 9 November 2026. What is here is what was found by reading the phone's own device
tree, the iOS 16.5 binaries in the common tree's shared cache, and the phone's own boot logs.
**No code was written**: two facts are still missing, and they are named below.

## 1. Where the value comes from, and where it goes

The path is **not** a HID device and not `AppleBacklightDriver`. It is:

```
Settings' slider
  -> CoreBrightness
     -> the display service (a registry entry for the framebuffer)
        -> IOMobileFramebufferUserClient, an external method
           -> the panel's backlight
```

CoreBrightness fails on the phone today, and the log says exactly where:

```
failed to retrieve registry ID for display service %u
```

(that string is in `CoreBrightness` in the shared cache, and appears verbatim in
`logs/xnu-c4-300-handoff.log:1507`, from backboardd). The words around it in the same binary
are `display service %u | registry ID 0x%llX | class %{public}@`,
`no display service found - waiting for display arrival`, `no backlight found` and
`invalid backlight service`.

## 2. What the iOS side expects of the framebuffer

`IOMobileFramebuffer.framework` (iOS 16.5, in the shared cache) exports
**`IOMobileFramebufferSwapSetBrightness`**, and the strings in it name the whole contract:

```
SwapSetBrightness: Brightness control is not enabled
SwapSetBrightnessLimit: Brightness control is not enabled
SwapSetDisplayEdr: Brightness control is not enabled
matched IOMFB (%p) to BC (%p) for embedded panel
BCBrtControlSetNits
BCBrtControlGetMaxNits
BCBrtControlGetMinNits
```

So brightness is a **swap-time external method** on the framebuffer's user client, and it is
guarded by a "brightness control is enabled" flag. The strings say the flag is *not* set:
`Brightness control is not enabled` is what the driver answers when it is asked to set
brightness and the flag is off.

CoreBrightness also names the service it expects (`AppleARMBacklight`, compared as a class name
in five places) and the selectors it calls (`setBrightness:`, `setBrightness:fadeSpeed:commit:`,
`setBrightnessFactor:`, `setBrightnessCap:`, `setBrightnessLimit:`,
`setBrightnessControlDisabled:`). backboardd sets `DisplayBrightnessFactor` on the phone
(`logs/kernel_clean.log:1584`, `logs/xnu-c4-400-window.log:1686`).

## 3. What the XNU side has, and does not, have

`refs/hakios-xnu-kernel/xnu/files/iokit/Kernel/MiniIOMobileFramebuffer.cpp`:

| what | where |
|---|---|
| the class | `IOMobileFramebuffer` (:361) |
| its user client | `IOMobileFramebufferUserClient` (:1683) |
| the selector table | `kFBMethod*` (:64-90) |
| the service's name | `setName("disp0")` (:689, :2038) |

It implements **no brightness selector** and publishes **no property saying that brightness
control is enabled**. That is why CoreBrightness finds no display service to bind to: the
service it matches on is there, but nothing tells it that the brightness control of that
service exists.

## 4. The hardware: the backlight is on the display PWM, not the general one

From the phone's own device tree (`refs/fdt-x6739.dtb`, read with `tools/mtk_fdt.py`):

| node | what it says |
|---|---|
| `/soc/pwm@1100e000` | `compatible = "mediatek,disp_pwm0", "mediatek,mt6885-disp-pwm"`; `reg` = **0x1100e000**, size 0x1000; `#pwm-cells = <2>`; `clocks` = 3; `clock-names = "main","mm","pwm_src"`; `phandle = 0x1c6` |
| `/soc/mtk_leds` | `compatible = "mediatek,disp-leds"`, `phandle = 0x1ca` |
| `/soc/mtk_leds/backlight` | `label = "lcd-backlight"`; `max-brightness = 255`; `max-hw-brightness = 255`; `led_mode = 4` (PWM); `gate_enable = 1`; `led-bits = 9` (9-bit); `pwm_config = <0 0 1 0 0 0>` |
| `/soc/dsi@1400e000/panel1@0` | the panel: `nt37703,fhdp,dsi,vdo` (the real panel is a Tianma NT37705, `docs/mt6893-facts.md:80`) |

**This is the finding that matters.** The backlight is *not* on `/soc/pwm@11006000` (that node
is `compatible = "mediatek,pwm"`, the general four-channel PWM block). It is on the **display
PWM** at `0x1100e000` (`mediatek,mt6885-disp-pwm`, the same family as
`drivers/pwm/pwm-mtk-disp.c` upstream), with MediaTek's LED layer on top
(`mediatek,disp-leds`, `led_mode = 4` = PWM, `led-bits = 9`).

The `backlight` node has no `pwm` property pointing at the PWM: that is normal for MediaTek,
where the LED driver knows `disp_pwm0` by its compatible string rather than by phandle.

## 5. The two facts still missing

Both were left open on purpose rather than guessed.

**a. The `mt6885` display-PWM register offsets — FOUND.** The MT6893's own Linux tree is on
this machine (`/mnt/project/HoshimaticProject/.mt6893_index`), and
`drivers/pwm/pwm-mtk-disp.c` has:

```c
{.compatible = "mediatek,mt6885-disp-pwm", .data = &mt6799_pwm_data},
```

so the mt6885 uses the mt6799's layout:

| register | offset | what |
|---|---|---|
| `DISP_PWM_EN` | `0x00` | the enable |
| `con0` | `0x18` | the clock divider (`PWM_CLKDIV_MASK`, divider at bit 16, 10 bits) |
| `con1` | `0x1C` | the period (bits 0-11) and the high width (bits 16-28) |
| `commit` | `0x0C` | the commit (mask `0x1`) — written after `con0`/`con1` |

The driver's own arithmetic (its comment gives the formulas):

```
period_ns = 10^9 * (clk_div + 1) * (period + 1) / PWM_CLK_RATE
duty_ns   = 10^9 * (clk_div + 1) * high_width / PWM_CLK_RATE
value = period | (high_width << 16)
```

and the LED layer's mapping of the brightness on top of it
(`drivers/misc/mediatek/leds/leds-mtk-disp.c:229`):

```c
led_Level = (((1 << led_bits) - 1) * level + (((1 << trans_bits) - 1) / 2))
            / ((1 << trans_bits) - 1);
```

with `led_bits = 9` and `trans_bits = 8` from the phone's own device tree: the 0..255 level
becomes a 9-bit number, `((511 * level) + 127) / 255`.

**b. The external-method selector number for `SwapSetBrightness` — still missing.** The
selector table is a numbered list (`kFBMethod*`); the framework's exported function is what a
number maps to. `llvm-objdump` refuses the reconstructed Mach-O from the shared cache
(`LC_BUILD_VERSION_COMMAND has incorrect cmdsize`); `r2` can read it with `-e io.va=false`.

## 6. The plan, once those two are known

1. A driver in `hakios-x6739/kernel/bsd/dev/` (the shape of `hakios.ft3681.c`: boot-arg gated,
   reading only first) that maps `mediatek,disp_pwm0` at `0x1100e000` and sets the backlight's
   duty cycle. It joins the common tree's `drivers/<sub>/conf`.
2. `MiniIOMobileFramebuffer.cpp` in the common tree: the missing external method (the selector
   from (b)), and the property that says the brightness control is enabled, so CoreBrightness
   can bind to the display service.
3. Build with `THEOS=/mnt/mainline/theos tools/build-kernel-linux.sh` (exit status 2 is
   expected; the script's `--judge` step decides).

## 7. What this means for "the slider is easy"

The slider is not a driver problem: the hardware is identified, and the iOS side is understood.
It is one missing constant (the selector number), one driver to write and one framebuffer
method to add. The hardware side is now fully known: the display PWM at `0x1100e000`, the
register offsets, the duty-cycle arithmetic and the 9-bit brightness mapping. What is left of
"easy" is the iOS-side method number, and then the writing.
