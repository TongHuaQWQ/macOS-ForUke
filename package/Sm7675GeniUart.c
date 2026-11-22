/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* GCC QUPv3 wrap1 SE2 (UART10) RCG sequence from gcc-cliffs.c / clk-rcg2.c. */
#include "Sm7675GeniUart.h"
#ifdef KERNEL
#include <sys/errno.h>
#else
#include <errno.h>
#endif

int sm7675_geni_uart10_source(const struct sm7675_geni_clock_io *io, uint32_t baud, uint32_t *hz) {
    if (!io || !io->read || !io->write || !io->delay_us || !hz) return -EINVAL;
    uint32_t m, n, rate;
    switch (baud) {
        case 115200: m = 384; n = 15625; rate = 7372800; break;
        case 3000000: m = 4; n = 25; rate = 48000000; break;
        case 3200000: m = 64; n = 375; rate = 51200000; break;
        case 4000000: m = 16; n = 75; rate = 64000000; break;
        default: return -EINVAL;
    }
    const uint32_t root = SM7675_GCC_QUP1_S2_CMD_RCGR;
    /* This SE is exclusive to Bluetooth UART10; manual RCG mode owns its source. */
    uint32_t dfs = io->read(io->context, root + 0x14);
    io->write(io->context, root + 0x14, dfs & ~1u);
    if (io->read(io->context, root + 0x14) & 1u) return -EIO;
    io->write(io->context, root + 8, (io->read(io->context, root + 8) & ~0xffffu) | m);
    io->write(io->context, root + 12, (io->read(io->context, root + 12) & ~0xffffu) | (~(n - m) & 0xffffu));
    io->write(io->context, root + 16, (io->read(io->context, root + 16) & ~0xffffu) | (~n & 0xffffu));
    uint32_t cfg = io->read(io->context, root + 4);
    /* HID_DIV=1 (/1), SRC_SEL=6 (GPLL0_OUT_EVEN 300 MHz), MODE=2 (dual-edge M/N). */
    cfg = (cfg & ~(0x1fu | 0x700u | 0x3000u | 0x100000u)) | 1u | (6u << 8) | (2u << 12);
    io->write(io->context, root + 4, cfg);
    io->write(io->context, root, io->read(io->context, root) | 1u);
    for (unsigned i = 0; i < 1000; i++) {
        if (!(io->read(io->context, root) & 1u)) { *hz = rate; return 0; }
        io->delay_us(io->context, 1);
    }
    return -ETIMEDOUT;
}
