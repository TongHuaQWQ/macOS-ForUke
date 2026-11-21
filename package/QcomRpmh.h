/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* RPMh for a remote processor's proxy votes: find a resource in the AOP command database (cmd-db) and turn a level into the index the
 * ARC resource takes, and describe the register writes of one active-only request in a TCS of the application processor's RSC, for
 * both register layouts (the RSC's major version 3 moved the TCS registers). Nothing here touches hardware: the owner applies the
 * writes and waits for the TCS to report itself done.
 *
 * The write sequences below are mainline Linux's, register for register: __tcs_buffer_write() and __tcs_set_trigger() of
 * drivers/soc/qcom/rpmh-rsc.c. That ordering is not cosmetic. The AMC mode bits have to be cleared trigger-first and enable-second,
 * and the 2026-11-21 usb6 boot is the evidence: the previous version of this file cleared both in a single write, and the boot
 * stopped on the fifth vote with nothing printed -- the RSC had stopped answering on the bus, which takes the whole kernel with it. */
#ifndef QCOM_RPMH_H
#define QCOM_RPMH_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define QCOM_CMD_DB_MAGIC 0x0c0330dbu
#define QCOM_RPMH_MAX_LEVELS 16u
/* The mask of a write that replaces the whole register; any other mask makes it a read-modify-write, which is how the AMC mode bits
 * are cleared one at a time. */
#define QCOM_RPMH_WHOLE_WORD 0xffffffffu

typedef struct {
    uint32_t address;
    uint16_t levels[QCOM_RPMH_MAX_LEVELS];
    unsigned level_count;
} QcomRpmhResource;

enum {QcomRpmhOk = 0, QcomRpmhNoDb = -1, QcomRpmhMissing = -2, QcomRpmhCorrupt = -3, QcomRpmhNoLevel = -4, QcomRpmhArgument = -5};

/* Looks name (up to 8 characters) up in a copy of the command database; levels are the ARC's voltage levels (aux data) when it has
 * them. */
int qcom_cmd_db_find(const uint8_t *db, size_t bytes, const char *name, QcomRpmhResource *out);
/* The ARC level index for a request of at least level (the highest index when level is above all of them; 0 turns the vote off). */
int qcom_rpmh_level_index(const QcomRpmhResource *r, uint32_t level, uint32_t *index);

/* One register write. `offset` is relative to the TCS block (DRV base + qcom,tcs-offset) and already carries the TCS stride where the
 * register lives inside a TCS; a WHOLE_WORD mask writes `value`, any other mask leaves the bits outside it alone. */
typedef struct {uint32_t offset, value, mask;} QcomRpmhWrite;

/* The register layout of an RSC DRV; offsets are relative to the TCS block (DRV base + tcs-offset). */
typedef struct {
    uint32_t tcs_stride, cmd_stride, irq_enable, irq_status, irq_clear, wait_for_cmpl, control, status, cmd_enable, cmd_msgid, cmd_addr,
        cmd_data, cmd_status;
} QcomRscLayout;

/* The layout for a DRV whose id register reads drv_id (major version in bits 23:16). */
QcomRscLayout qcom_rsc_layout(uint32_t drv_id);
/* The writes of an active request of count (<= 4) data commands in TCS tcs, in order: the buffer, then the AMC mode bits in the
 * order the hardware requires. `wait` asks for a response, which is what makes CMD_STATUS say a command was issued and completed;
 * without it the request is mainline's RPMH_ACTIVE_ONLY_STATE batch and only the TCS itself reports when it is free again. Returns
 * the number of writes, 0 on bad arguments. */
unsigned qcom_rpmh_active_request(const QcomRscLayout *l, unsigned tcs, const uint32_t *addresses, const uint32_t *values, unsigned count,
                                  int wait, QcomRpmhWrite *writes, unsigned capacity);
/* The writes of the TCS registers once its request is done. */
unsigned qcom_rpmh_finish(const QcomRscLayout *l, unsigned tcs, QcomRpmhWrite *writes, unsigned capacity);

#ifdef __cplusplus
}
#endif
#endif
