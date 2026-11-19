/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/*
 * USB2 device-mode board bring-up for SM7675 (uke / cliffs).
 *
 * Resources, and where each value comes from:
 *
 *   GCC               0x00100000, window 0x53000   core GDSC 0x39004 bit31, BCR 0x39000 bit0,
 *                                                  link clocks 0x39088/0x39018/0x39024/0x39028/0x3908c,
 *                                                  core clock select 0x39030 (0x71f -> 0x105), 0x3902c
 *   TCSR (tcsrcc)     0x01f40000 + 0x000b1118 = 0x01ff1118   USB2 clkref gate, bit 0
 *                     (the OEM tcsrcc-pineapple driver uses an MMIO regmap; a SCM request for the
 *                      same register answers -2 on this SEC firmware, measured 2026-11-15)
 *   apps RSC          0x017a2000 + ... : cmd-db 0x81c60000 (0x20000), RSC 0x17a20000 (0x10000), DRV-2 TCS 2
 *                     rails: ldob2 880 mV, ldob4 1200 mV, ldob7 1800 mV, ldob17 3075 mV (+ ldob3 for SS),
 *                     plus xo.lvl=3 and the SN2 bandwidth vote (60 MB/s at full speed)
 *                     RSC version must be major 3 (this board's DRV-2 reports 0x00030200 = 3.2)
 *   PMIC arbiter      0xc400000 (0x3000), owner 0xc42d000 (0x4000), observer 0xc440000, channel 0xc500000;
 *                     PM7550BA SID 7 repeater at 0xfd00, OEM device/host tuning tables
 *
 * No SCM, no SPMI driver and no display dependency: everything is MMIO through the arbiter windows above,
 * which is why it ports unchanged from the same-device upstream
 * (Allenwdk/UniXNU-uke package/UkeUsbBoard.cpp, docs/USB2-20261115.md).
 *
 * Failure codes are upstream's: -1 GCC window, -2 rails/RSC vote, -3 core GDSC, -4 clocks / clkref / reset,
 * -6 PMIC repeater. They are reported through the component's own status so the panel's QCOMUSB line shows
 * which stage stopped.
 */
#pragma once

int qcom_usb_board_init(void);
