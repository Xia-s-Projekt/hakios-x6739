#!/bin/bash
# The X6739's firmware, out of YOUR phone's Android, into fw-*/ next to this repository
# (git ignores them: they are the vendor's, and nothing of the vendor's is in the repository).
#
#   fw-touch/focaltech_ts_fw_00.bin   the touch controller's (FocalTech): the loader carries it
#                                     (mtkloader/tcm.h; mtkloader/build.sh makes touchfw.h out of it)
#   fw-connsys/                       CONNSYS's, for Wi-Fi and Bluetooth: this board's files are
#                                     the soc3_0_* ones, not the reference board's soc1_0_*
#   fw-gpu/valhall-1691526.wa         the Mali-G77's firmware (the "valhall" generation)
#
# Android keeps them where the shell user cannot read (SELinux), so this needs root on the phone
# (Magisk's su): they are copied to a temporary folder, pulled, and the folder is removed. Read only:
# nothing on the phone is changed.
set -e
S=${HAKIOS_SERIAL:?set HAKIOS_SERIAL to the serial of your phone, as adb devices shows it}
OUT="${1:-$(cd "$(dirname "$0")" && pwd)}"
TMP=/data/local/tmp/hakios-fw

# CONNSYS: this board's Wi-Fi/Bluetooth firmware is the soc3_0 generation (the reference
# board's was soc1_0). The list is what the phone is known to have (docs/mt6893-facts.md)
# plus the files already in fw-connsys/ that the list was missing (the "wmmcu" pair: the
# multimedia MCU's RAM code and its patch). The pull is a copy, so a name that is not on
# the phone is skipped without an error.
CONNSYS="soc3_0_patch_mcu_1a_1_hdr.bin soc3_0_patch_wmmcu_1a_1_hdr.bin
	soc3_0_ram_mcu_1a_1_hdr.bin soc3_0_ram_mcu_1_1_hdr.bin soc3_0_ram_mcu_e1_hdr.bin
	soc3_0_ram_wmmcu_1a_1_hdr.bin soc3_0_ram_wifi_1a_1_hdr.bin
	soc3_0_ram_bt_1a_1_hdr.bin soc3_0_ram_bt_1_1_hdr.bin WIFI_RAM_CODE_soc3_0_1a_1.bin
	WMT_SOC.cfg wifi.cfg BT_FW.cfg conninfra.cfg"

# The touch controller's firmware: FocalTech on this phone, not the reference board's OmniVision.
TOUCH=focaltech_ts_fw_00.bin
# The GPU's: the Mali-G77 ("valhall") generation.
GPU=valhall-1691526.wa

echo "pulling the firmware off $S (read only)..."

adb -s "$S" shell "su -c 'rm -rf $TMP; mkdir -p $TMP; \
	for f in $(echo $CONNSYS); do \
		if [ -f /vendor/firmware/\$f ]; then cp /vendor/firmware/\$f $TMP/; fi; \
	done; \
	[ -f /vendor/firmware/$TOUCH ] && cp /vendor/firmware/$TOUCH $TMP/; \
	[ -f /vendor/firmware/$GPU ] && cp /vendor/firmware/$GPU $TMP/; \
	chmod 644 $TMP/* 2>/dev/null; true'"

mkdir -p "$OUT/fw-touch" "$OUT/fw-connsys" "$OUT/fw-gpu"
adb -s "$S" pull "$TMP/$TOUCH" "$OUT/fw-touch/" >/dev/null
adb -s "$S" pull "$TMP/$GPU" "$OUT/fw-gpu/" >/dev/null
for f in $CONNSYS; do
	adb -s "$S" pull "$TMP/$f" "$OUT/fw-connsys/" >/dev/null 2>&1 || true
done

adb -s "$S" shell "su -c 'rm -rf $TMP'"

echo
echo "what came off the phone:"
ls -l "$OUT/fw-touch" "$OUT/fw-connsys" "$OUT/fw-gpu"
