/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef SM7675_GENI_UART_H
#define SM7675_GENI_UART_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Verified against Xiaomi Pad 7 (uke / SM7675 "cliffs") live-os4.dts, gcc-cliffs.c and cliffs TLMM:
 *   UART: QUPv3_1 SE2 (qupv3_se10_4uart) @ 0x00888000, wrapper @ 0x008c0000
 *   IRQ:  GIC SPI 584 (0x248) -> INTID 616; TLMM summary SPI 208 (0xd0) -> INTID 240
 *   DMA:  apps_smmu@15000000 stream 0x423
 *   Pins: CTS=40, RTS=41, TX=42, RX=43 (qup1_se2_l0..l3, func 1), SW_CTRL=80, BT_EN=156
 *   GCC:  GPLL0 vote 0x52020 bit 0; QUP1 vote 0x52010 (bus 0xf, SE2 bit 6 = 0x40);
 *         SE2 CBCR 0x1e274; SE2 CMD_RCGR 0x1e280 (parent 6 = GPLL0_OUT_EVEN 300 MHz). */
#define SM7675_UART10_BASE 0x00888000ULL
#define SM7675_QUP1_BASE 0x008c0000ULL
#define SM7675_UART10_INTID 616u
#define SM7675_QUP1_SID 0x423u
#define SM7675_TLMM_BASE 0x0f000000ULL
#define SM7675_TLMM_SIZE 0x00100000ULL
#define SM7675_TLMM_SUMMARY_INTID 240u
#define SM7675_GCC_BASE 0x00100000ULL
#define SM7675_GCC_GPLL0_VOTE_REG 0x52020u
#define SM7675_GCC_QUP1_VOTE_REG 0x52010u
#define SM7675_GCC_QUP1_S2_CBCR 0x1e274u
#define SM7675_GCC_QUP1_S2_CMD_RCGR 0x1e280u
#define SM7675_GCC_QUP1_BUS_MASK 0x0000000fu
#define SM7675_GCC_QUP1_S2_BIT (1u << 6)
/* Other QUPv3 wrap1 users sharing APCS vote register 0x52010:
 * s0(4), s1(5), s3(7), s4(8), s5(9), s6(10), s7(17), ibi2/3 ahb+ctrl(25..28). */
#define SM7675_GCC_QUP1_OTHER_MASK 0x1e0207b0u
#define SM7675_BT_GPIO_CTS 40u
#define SM7675_BT_GPIO_RTS 41u
#define SM7675_BT_GPIO_TX 42u
#define SM7675_BT_GPIO_RX 43u
#define SM7675_BT_GPIO_SW_CTRL 80u
#define SM7675_BT_GPIO_EN 156u
#define SM7675_BT_UART_BAUD 3000000u

struct sm7675_geni_clock_io {
    void *context;
    uint32_t (*read)(void *, uint32_t);
    void (*write)(void *, uint32_t, uint32_t);
    void (*delay_us)(void *, unsigned);
};
/* UART engine is stopped before source changes. GPLL0-even is 300 MHz. */
int sm7675_geni_uart10_source(const struct sm7675_geni_clock_io *, uint32_t baud, uint32_t *hz);
/* Serialize shared GCC votes from worker context. Returns false on allocation failure. */
bool sm7675_bt_gcc_update32(volatile uint32_t *, uint32_t clear, uint32_t set);
/* Own QUP1 SE2 and its newly acquired shared core/AHB votes in 0x52010. */
bool sm7675_geni_uart10_vote(volatile uint32_t *, bool enabled);
#ifdef __cplusplus
}
#endif
#endif
