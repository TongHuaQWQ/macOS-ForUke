/** @file
  A tiny stage log in DRAM that survives a reset.

  Every device round so far ended the same way: the panel shows something, then it
  goes black, and there is no way to tell whether the firmware died, the display
  clock was repowered by a vendor driver, or the boot actually continued into the
  kernel. This header gives the board's own drivers one place to record "I got this
  far", in memory that nothing else uses and that is not cleared by a reset, so the
  *next* boot can print it before the display is at risk.

  The window is "Log Buffer" in Library/MemoryMapLib (0xA7605000, 32 KiB, ours).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef UKE_PERSIST_H_
#define UKE_PERSIST_H_

#include <Uefi.h>
#include <Library/BaseLib.h>

/* "Log Buffer" in Library/MemoryMapLib: 0xA7605000, 32 KiB, RtData, ours, mapped
 * write-back and marked execute-never. The FD-Reserved page at 0xA7000000 was the first
 * choice, but two rounds died on a synchronous exception whose reported address,
 * 0xA700DFAC, sits inside that page - so this record keeps clear of it. */
#define UKE_PERSIST_BASE      0xA7605000ULL
#define UKE_PERSIST_MAGIC     0x314B4555U /* "UEK1" */
#define UKE_PERSIST_RECORDS   32
#define UKE_PERSIST_HEADER    0x40

//
// Stage identifiers. Keep them append-only: a value already in DRAM belongs to the
// previous boot and has to stay readable.
//
#define UKE_STAGE_PROGRESS_ENTRY   1
#define UKE_STAGE_PROGRESS_READY   2  /* heartbeat driver finished its entry */
#define UKE_STAGE_LOG_ENTRY        3
#define UKE_STAGE_LOG_READY_TO_BOOT 4
#define UKE_STAGE_LOG_PROBED       5
#define UKE_STAGE_LOG_DONE         6
#define UKE_STAGE_BOOT_MEMORY      7

typedef struct {
  UINT32  Stage;
  UINT32  Detail;
  UINT64  Tick;
} UKE_PERSIST_RECORD;

typedef struct {
  UINT32              Magic;
  UINT32              Version;
  UINT32              Boot;
  UINT32              Count;      /* records written this boot */
  UINT32              LastStage;
  UINT32              LastDetail;
  UKE_PERSIST_RECORD  Records[UKE_PERSIST_RECORDS];
} UKE_PERSIST_BLOCK;

STATIC inline UKE_PERSIST_BLOCK *
UkePersistBlock (
  VOID
  )
{
  return (UKE_PERSIST_BLOCK *)(UINTN)UKE_PERSIST_BASE;
}

/** Append a stage record. Safe to call from any firmware driver; never faults on a
 *  cold DRAM because the window is mapped by MemoryMapLib and not otherwise used. */
STATIC inline VOID
UkePersistStage (
  IN UINT32  Stage,
  IN UINT32  Detail
  )
{
  UKE_PERSIST_BLOCK  *Block;

  Block = UkePersistBlock ();
  if (Block->Magic != UKE_PERSIST_MAGIC) {
    Block->Magic   = UKE_PERSIST_MAGIC;
    Block->Version = 1;
    Block->Boot    = 0;
    Block->Count   = 0;
  }

  Block->Records[Block->Count % UKE_PERSIST_RECORDS].Stage  = Stage;
  Block->Records[Block->Count % UKE_PERSIST_RECORDS].Detail = Detail;
  /* No counter here: AsmReadCntpct lives in ArmLib, and this header is included by
   * drivers that do not link it. The tick is diagnostic only, so a zero is enough. */
  Block->Records[Block->Count % UKE_PERSIST_RECORDS].Tick   = 0;
  Block->Count     += 1;
  Block->LastStage  = Stage;
  Block->LastDetail = Detail;

  /* Normal memory, read back by a later boot; a compiler barrier keeps the stores. */
  __asm__ volatile ("" ::: "memory");
}

#endif /* UKE_PERSIST_H_ */
