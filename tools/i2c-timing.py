#!/usr/bin/env python3
"""The I2C0 timing registers of the MT6893, computed the way the vendor's driver does.

A port of Linux drivers/i2c/busses/i2c-mt65xx.c (6.1): mtk_i2c_set_speed ->
mtk_i2c_calculate_speed -> mtk_i2c_check_ac_timing, for the mt6873_compatible
(mediatek,mt6873-i2c, which is what this phone's tree says) with
timing_adjust = 1, ltiming_adjust = 1, speed_ver = 0 and clk_src_div = 1.

The registers it fills in are the ones the driver writes in mtk_i2c_init_hw:
TIMING (0x20), HS (0x30), LTIMING (0x2c), EXT_CONF (0x28), CLOCK_DIV (0x48),
SDA_TIMING (0x3c).

Usage: i2c-timing.py [parent_clk_hz] [target_hz]
"""
import sys

MAX_SAMPLE_CNT_DIV = 8
MAX_STEP_CNT_DIV = 64
MAX_CLOCK_DIV_5BITS = 32

STANDARD = 100000
FAST = 400000
FMP = 1000000
HS = 3400000

def spec(speed):
    # struct i2c_spec_values, with the buffers the driver adds
    if speed <= STANDARD:
        return dict(min_low_ns=4700 + 1000 // 3, min_su_sta_ns=4700 + 1000 // 3,
                    max_hd_dat_ns=3450 - 1000 // 3, min_su_dat_ns=250 + 1000 // 3)
    if speed <= FAST:
        return dict(min_low_ns=1300 + 300 // 3, min_su_sta_ns=600 + 300 // 3,
                    max_hd_dat_ns=900 - 300 // 3, min_su_dat_ns=100 + 300 // 3)
    if speed <= FMP:
        return dict(min_low_ns=500 + 20 // 3, min_su_sta_ns=260 + 20 // 3,
                    max_hd_dat_ns=400 - 20 // 3, min_su_dat_ns=50 + 20 // 3)
    return dict(min_low_ns=160 + 10 // 3, min_su_sta_ns=160 + 10 // 3,
                max_hd_dat_ns=70 - 10 // 3, min_su_dat_ns=10 + 10 // 3)

def max_step_cnt(speed):
    return 8 if speed > FMP else MAX_STEP_CNT_DIV

def div_round_up(a, b):
    return -(-a // b)

def clk_div_restri(inter_clk_div, sample_cnt):
    # mtk_i2c_get_clk_div_restri, ltiming_adjust = 1
    if sample_cnt == 1:
        return 0 if inter_clk_div == 0 else 1
    if inter_clk_div == 0:
        return -1
    if inter_clk_div == 1:
        return 0
    return 1

def check_ac_timing(clk_src, check_speed, step_cnt, sample_cnt):
    """The driver's mtk_i2c_check_ac_timing: (0, ac) on success, (negative, None) otherwise."""
    ac = {}
    sample_ns = (1000000000 * (sample_cnt + 1)) // clk_src
    max_sta_cnt = 0x100                       # ltiming_adjust
    if check_speed > FMP:
        max_sta_cnt = 0x80
    sp = spec(check_speed)
    clk_ns = 1000000000 // clk_src             # ltiming_adjust
    su_sta_cnt = div_round_up(sp['min_su_sta_ns'], clk_ns)
    if su_sta_cnt > max_sta_cnt:
        return -1, None
    low_cnt = div_round_up(sp['min_low_ns'], sample_ns)
    msc = max_step_cnt(check_speed)
    if check_speed > FMP:
        if (step_cnt > 1) and (step_cnt < msc - 1):
            high_cnt = step_cnt - 1
            low_cnt = step_cnt + 1
        else:
            return -2, None
    else:
        if (2 * step_cnt) > low_cnt and low_cnt < msc:
            if low_cnt > step_cnt:
                high_cnt = 2 * step_cnt - low_cnt
            else:
                high_cnt = step_cnt
                low_cnt = step_cnt
        else:
            return -2, None
    sda_max = sp['max_hd_dat_ns'] // sample_ns
    if sda_max > low_cnt:
        sda_max = 0
    sda_min = div_round_up(sp['min_su_dat_ns'], sample_ns)
    if sda_min < low_cnt:
        sda_min = 0
    if sda_min > sda_max:
        return -3, None
    if check_speed > FMP:
        ac['hs'] = 0x0003 | 0x0080 | (sample_cnt << 12) | (high_cnt << 8)
        ac['ltiming'] = (sample_cnt << 6) | low_cnt
        ac['ext'] = (su_sta_cnt << 8) | 1
        ac['sda_timing'] = 0
    else:
        ac['hs'] = 0
        ac['htiming'] = (sample_cnt << 8) | high_cnt
        ac['ltiming'] = (sample_cnt << 6) | low_cnt
        ac['ext'] = (su_sta_cnt << 8) | 1
        ac['sda_timing'] = (1 << 12) | (sda_max + sda_min) // 2
    return 0, ac

def calculate_speed(clk_src, target_speed, inter_clk_div, verbose=False):
    """The driver's mtk_i2c_calculate_speed: (step_cnt, sample_cnt, ac) or None.

    The ac_timing it leaves behind is the one the driver's check_ac_timing call
    inside the loop filled in, with the decremented counts.
    """
    best_mul = MAX_SAMPLE_CNT_DIV * max_step_cnt(target_speed)
    opt_div = div_round_up(clk_src >> 1, target_speed)
    base_sample = MAX_SAMPLE_CNT_DIV
    base_step = max_step_cnt(target_speed)
    found = False
    ac = None
    for sample_cnt in range(1, MAX_SAMPLE_CNT_DIV + 1):
        restri = clk_div_restri(inter_clk_div, sample_cnt)
        step_cnt = div_round_up(opt_div + restri, sample_cnt)
        cnt_mul = step_cnt * sample_cnt
        if step_cnt > max_step_cnt(target_speed):
            continue
        if cnt_mul < best_mul:
            ret, new_ac = check_ac_timing(clk_src, target_speed, step_cnt - 1, sample_cnt - 1)
            if verbose:
                print(f'  sample={sample_cnt} step={step_cnt} mul={cnt_mul} check={ret}')
            if ret:
                continue
            best_mul = cnt_mul
            base_sample = sample_cnt
            base_step = step_cnt
            ac = new_ac
            found = True
            if best_mul == (opt_div + restri):
                break
    if not found:
        return None
    sample_cnt = base_sample
    step_cnt = base_step
    restri = clk_div_restri(inter_clk_div, sample_cnt)
    if (clk_src // (2 * (sample_cnt * step_cnt - restri))) > target_speed:
        return None
    return step_cnt - 1, sample_cnt - 1, ac

def set_speed(parent_clk, target_speed, clk_src_div=1, verbose=False):
    """The driver's mtk_i2c_set_speed: the registers and the clock divider."""
    parent_clk //= clk_src_div
    for clk_div in range(1, MAX_CLOCK_DIV_5BITS + 1):
        clk_src = parent_clk // clk_div
        inter = clk_div - 1
        r = calculate_speed(clk_src, target_speed, inter, verbose)
        if r is None:
            continue
        step_cnt, sample_cnt, ac = r
        out = dict(clk_div=clk_div, clk_src=clk_src,
                   timing_reg=(sample_cnt << 8) | step_cnt,
                   inter_clk_div=((clk_div - 1) << 8) | (clk_div - 1))
        out.update(ac)
        # the driver writes TIMING, HS and LTIMING twice: once from timing_reg /
        # high_speed_reg / ltiming_reg, and once, inside timing_adjust, from
        # ac_timing.htiming / ac_timing.hs / ac_timing.ltiming. The second write
        # is the one that stays.
        out['htiming'] = out.get('htiming', 0)
        out['ltiming'] = out.get('ltiming', 0)
        return out
    return None

NAMES = (('htiming', 'TIMING        (0x20)'),
         ('hs', 'HS            (0x30)'),
         ('ltiming', 'LTIMING       (0x2c)'),
         ('ext', 'EXT_CONF      (0x28)'),
         ('inter_clk_div', 'CLOCK_DIV     (0x48)'),
         ('sda_timing', 'SDA_TIMING    (0x3c)'))

if __name__ == '__main__':
    parent = int(sys.argv[1]) if len(sys.argv) > 1 else 124800000
    target = int(sys.argv[2]) if len(sys.argv) > 2 else 400000
    r = set_speed(parent, target, verbose=True)
    if r is None:
        print('no timing found')
        raise SystemExit(1)
    print(f'parent {parent} target {target} clk_div {r["clk_div"]} clk_src {r["clk_src"]}')
    for k, name in NAMES:
        print(f'  {name} = 0x{r[k]:04x}  ({r[k]})')
